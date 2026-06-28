#define SDL_MAIN_USE_CALLBACKS 1
#define VK_NO_PROTOTYPES
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <vulkan/vulkan.h>

#include <memory>
#include <source_location>
#include <span>
#include <SDL3/SDL_main.h>
#include "SDL3/SDL_log.h"
#include "SDL3/SDL_timer.h"
#include "SDL3/SDL_vulkan.h"
#include <include/gpu/vk/VulkanBackendContext.h>
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanPreferredFeatures.h>
#include <VkBootstrap.h>
#include "src/gpu/GpuTypesPriv.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/vk/VulkanGraphiteContext.h"
#include "include/gpu/graphite/vk/VulkanGraphiteTypes.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/BackendSemaphore.h"
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"
#include "include/gpu/graphite/Context.h"
#include "litehtml/document.h"
#include "litehtml/document_container.h"
#include "litehtml/html_tag.h"
#include <curl/curl.h>
#include <string>
#include <vector>

auto chkSDL(bool result, std::source_location loc = {}) {
    if (!result) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %s", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), SDL_GetError());
        std::exit(1);
    }
}

template<typename T>
auto chkVkb(vkb::Result<T> result, std::source_location loc = {}) -> T {
    if (!result) {
        auto message = result.error().message();
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %s (%d)", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), message.c_str(), result.vk_result());
        std::exit(1);
    }
    return result.value();
}

static auto chk(VkResult res, std::source_location loc = {}) -> void {
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "%s:%d:%d [%s]: %d", loc.file_name(), loc.line(), loc.column(),
                     loc.function_name(), res);
        std::exit(1);
    }
}

static auto curlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Fetch a URL over HTTP(S) into out. Returns false on failure.
static auto httpGet(const std::string &url, std::string &out) -> bool {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "codotaku-web/0.0.1");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_ERROR, "curl failed for %s: %s", url.c_str(), curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

// Resolve a possibly-relative reference against a base URL.
static auto resolveUrl(const std::string &base, const std::string &rel) -> std::string {
    if (rel.empty()) return base;
    if (rel.find("://") != std::string::npos) return rel; // already absolute
    if (rel.rfind("//", 0) == 0) {
        // protocol-relative
        auto schemeEnd = base.find("://");
        std::string scheme = schemeEnd != std::string::npos ? base.substr(0, schemeEnd) : "https";
        return scheme + ":" + rel;
    }
    auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return rel;
    auto hostStart = schemeEnd + 3;
    auto pathStart = base.find('/', hostStart);
    std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
    if (rel[0] == '/') return origin + rel; // root-relative
    // document-relative: take directory of base path
    std::string dir = pathStart == std::string::npos
                          ? origin + "/"
                          : base.substr(0, base.find_last_of('/') + 1);
    return dir + rel;
}

static constexpr int MAX_FRAMES = 2;

#include <include/core/SkFontMgr.h>
#include <include/ports/SkTypeface_win.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/effects/SkGradient.h>

struct SkiaFont {
    SkFont font;
    litehtml::font_metrics metrics;
};

class DocumentContainer : public litehtml::document_container {
    std::string baseUrl_;
    std::string pageUrl_;
    sk_sp<SkFontMgr> m_fontMgr;
    std::map<std::string, sk_sp<SkImage> > images_;

protected:
    int width_ = 800;
    int height_ = 600;

    // Resolve a reference against the most specific base available.
    std::string makeAbsolute(const std::string &src, const std::string &baseurl) const {
        const std::string &base = !baseurl.empty()
                                      ? baseurl
                                      : (!baseUrl_.empty() ? baseUrl_ : pageUrl_);
        return resolveUrl(base, src);
    }

public:
    DocumentContainer() {
        m_fontMgr = SkFontMgr_New_DirectWrite();
    }

    void set_page_url(const std::string &url) {
        pageUrl_ = url;
    }

    void set_size(int w, int h) {
        width_ = w;
        height_ = h;
    }

    litehtml::uint_ptr create_font(const litehtml::font_description &descr, const litehtml::document *doc,
                                   litehtml::font_metrics *fm) override {
        SkFontStyle style(descr.weight, SkFontStyle::kNormal_Weight,
                          descr.style == litehtml::font_style_italic
                              ? SkFontStyle::kItalic_Slant
                              : SkFontStyle::kUpright_Slant);

        sk_sp<SkTypeface> typeface;
        std::stringstream families(descr.family);
        std::string family;
        while (std::getline(families, family, ',')) {
            size_t start = family.find_first_not_of(" \t\"'");
            size_t end = family.find_last_not_of(" \t\"'");
            if (start == std::string::npos) continue;
            family = family.substr(start, end - start + 1);

            if (family == "sans-serif") family = "Arial";
            else if (family == "serif") family = "Times New Roman";
            else if (family == "monospace") family = "Consolas";
            else if (family == "cursive") family = "Comic Sans MS";

            typeface = m_fontMgr->matchFamilyStyle(family.c_str(), style);
            if (typeface) break;
        }
        if (!typeface)
            typeface = m_fontMgr->legacyMakeTypeface(nullptr, style);

        auto font = std::make_unique<SkiaFont>();
        font->font.setTypeface(typeface);
        font->font.setSize(descr.size);
        font->font.setSubpixel(true);
        font->font.setEdging(SkFont::Edging::kSubpixelAntiAlias);

        SkFontMetrics skfm;
        font->font.getMetrics(&skfm);

        fm->font_size = descr.size;
        fm->ascent = ceilf(-skfm.fAscent);
        fm->descent = ceilf(skfm.fDescent);
        fm->height = ceilf(-skfm.fAscent + skfm.fDescent + skfm.fLeading);
        fm->x_height = ceilf(skfm.fXHeight);
        fm->ch_width = ceilf(font->font.measureText("0", 1, SkTextEncoding::kUTF8));

        font->metrics = *fm;

        return reinterpret_cast<litehtml::uint_ptr>(font.release());
    }

    void delete_font(litehtml::uint_ptr hFont) override {
        delete reinterpret_cast<SkiaFont *>(hFont);
    }

    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr hFont) override {
        auto font = reinterpret_cast<SkiaFont *>(hFont);
        return static_cast<int>(ceilf(font->font.measureText(text, strlen(text), SkTextEncoding::kUTF8)));
    }

    void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr hFont, litehtml::web_color color,
                   const litehtml::position &pos) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);
        auto font = reinterpret_cast<SkiaFont *>(hFont);

        SkPaint paint;
        paint.setColor(SkColorSetARGB(color.alpha, color.red, color.green, color.blue));
        paint.setAntiAlias(true);

        canvas->drawSimpleText(text, strlen(text), SkTextEncoding::kUTF8, pos.x, pos.y + font->metrics.ascent,
                               font->font, paint);
    }

    litehtml::pixel_t pt_to_px(float pt) const override {
        return pt * 96.0f / 72.0f;
    }

    litehtml::pixel_t get_default_font_size() const override {
        return 16;
    }

    const char *get_default_font_name() const override {
        return "sans-serif";
    }

    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);
        SkPaint paint;
        paint.setColor(SkColorSetARGB(marker.color.alpha, marker.color.red, marker.color.green, marker.color.blue));
        paint.setAntiAlias(true);

        float r = marker.pos.width / 4.0f;
        float cx = marker.pos.x + marker.pos.width / 2.0f;
        float cy = marker.pos.y + marker.pos.height / 2.0f;

        switch (marker.marker_type) {
            case litehtml::list_style_type_circle:
                paint.setStyle(SkPaint::kStroke_Style);
                canvas->drawCircle(cx, cy, r, paint);
                break;
            case litehtml::list_style_type_disc:
                paint.setStyle(SkPaint::kFill_Style);
                canvas->drawCircle(cx, cy, r, paint);
                break;
            case litehtml::list_style_type_square:
                paint.setStyle(SkPaint::kFill_Style);
                canvas->drawRect(SkRect::MakeXYWH(cx - r, cy - r, r * 2, r * 2), paint);
                break;
            default:
                break;
        }
    }

    void load_image(const char *src, const char *baseurl, bool redraw_on_ready) override {
        std::string url = makeAbsolute(src, baseurl ? baseurl : "");
        if (images_.contains(url)) return;

        std::string data;
        if (httpGet(url, data) && !data.empty()) {
            auto skData = SkData::MakeWithCopy(data.data(), data.size());
            auto image = SkImages::DeferredFromEncodedData(skData);
            if (image)
                images_[url] = image;
        }
    }

    void get_image_size(const char *src, const char *baseurl, litehtml::size &sz) override {
        auto it = images_.find(makeAbsolute(src, baseurl ? baseurl : ""));
        if (it != images_.end()) {
            sz.width = it->second->width();
            sz.height = it->second->height();
        } else {
            sz.width = 0;
            sz.height = 0;
        }
    }

    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const std::string &url,
                    const std::string &base_url) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);
        auto it = images_.find(makeAbsolute(url, base_url));
        if (it != images_.end()) {
            canvas->drawImageRect(
                it->second, SkRect::MakeXYWH(layer.border_box.x, layer.border_box.y, layer.border_box.width,
                                             layer.border_box.height), SkSamplingOptions());
        }
    }

    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                         const litehtml::web_color &color) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);
        SkPaint paint;
        paint.setColor(SkColorSetARGB(color.alpha, color.red, color.green, color.blue));
        paint.setAntiAlias(true);

        canvas->drawRect(SkRect::MakeXYWH(layer.border_box.x, layer.border_box.y,
                                          layer.border_box.width, layer.border_box.height), paint);
    }

    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::linear_gradient &gradient) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);

        std::vector<SkColor4f> colors;
        std::vector<float> pos;
        for (const auto &cp: gradient.color_points) {
            colors.push_back(SkColor4f{
                cp.color.red / 255.0f, cp.color.green / 255.0f, cp.color.blue / 255.0f, cp.color.alpha / 255.0f
            });
            pos.push_back(cp.offset);
        }

        SkPoint pts[2] = {
            {gradient.start.x, gradient.start.y},
            {gradient.end.x, gradient.end.y},
        };

        SkGradient skGrad(SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp),
                          SkGradient::Interpolation());
        auto shader = SkShaders::LinearGradient(pts, skGrad);

        SkPaint paint;
        paint.setShader(shader);
        paint.setAntiAlias(true);

        canvas->drawRect(SkRect::MakeXYWH(layer.border_box.x, layer.border_box.y,
                                          layer.border_box.width, layer.border_box.height), paint);
    }

    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                              const litehtml::background_layer::radial_gradient &gradient) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);

        std::vector<SkColor4f> colors;
        std::vector<float> pos;
        for (const auto &cp: gradient.color_points) {
            colors.push_back(SkColor4f{
                cp.color.red / 255.0f, cp.color.green / 255.0f, cp.color.blue / 255.0f, cp.color.alpha / 255.0f
            });
            pos.push_back(cp.offset);
        }

        SkPoint center = {gradient.position.x, gradient.position.y};
        float radius = gradient.radius.x;

        SkGradient skGrad(SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp),
                          SkGradient::Interpolation());
        auto shader = SkShaders::RadialGradient(center, radius, skGrad);

        SkPaint paint;
        paint.setShader(shader);
        paint.setAntiAlias(true);

        canvas->drawRect(SkRect::MakeXYWH(layer.border_box.x, layer.border_box.y,
                                          layer.border_box.width, layer.border_box.height), paint);
    }

    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                             const litehtml::background_layer::conic_gradient &gradient) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);

        std::vector<SkColor4f> colors;
        std::vector<float> pos;
        for (const auto &cp: gradient.color_points) {
            colors.push_back(SkColor4f{
                cp.color.red / 255.0f, cp.color.green / 255.0f, cp.color.blue / 255.0f, cp.color.alpha / 255.0f
            });
            pos.push_back(cp.offset);
        }

        SkPoint center = {gradient.position.x, gradient.position.y};

        SkGradient skGrad(SkGradient::Colors(SkSpan(colors), SkSpan(pos), SkTileMode::kClamp),
                          SkGradient::Interpolation());
        auto shader = SkShaders::SweepGradient(center, skGrad);

        SkPaint paint;
        paint.setShader(shader);
        paint.setAntiAlias(true);

        canvas->drawRect(SkRect::MakeXYWH(layer.border_box.x, layer.border_box.y,
                                          layer.border_box.width, layer.border_box.height), paint);
    }

    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders, const litehtml::position &draw_pos,
                      bool root) override {
        auto canvas = reinterpret_cast<SkCanvas *>(hdc);

        auto draw_border = [&](const litehtml::border &b, float x, float y, float w, float h) {
            if (b.width <= 0 || b.style == litehtml::border_style_none || b.style == litehtml::border_style_hidden)
                return;
            SkPaint paint;
            paint.setColor(SkColorSetARGB(b.color.alpha, b.color.red, b.color.green, b.color.blue));
            paint.setAntiAlias(true);
            canvas->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
        };

        draw_border(borders.top, draw_pos.x, draw_pos.y, draw_pos.width,
                    borders.top.width);
        draw_border(borders.bottom, draw_pos.x, draw_pos.y + draw_pos.height - borders.bottom.width,
                    draw_pos.width, borders.bottom.width);
        draw_border(borders.left, draw_pos.x, draw_pos.y, borders.left.width,
                    draw_pos.height);
        draw_border(borders.right, draw_pos.x + draw_pos.width - borders.right.width, draw_pos.y,
                    borders.right.width, draw_pos.height);
    }

    void set_caption(const char *caption) override {
    }

    void set_base_url(const char *base_url) override {
        baseUrl_ = base_url;
    }

    void link(const std::shared_ptr<litehtml::document> &doc, const litehtml::element::ptr &el) override {
    }

    void on_anchor_click(const char *url, const litehtml::element::ptr &el) override {
    }

    void on_mouse_event(const litehtml::element::ptr &el, litehtml::mouse_event event) override {
    }

    void set_cursor(const char *cursor) override {
    }

    void transform_text(litehtml::string &text, litehtml::text_transform tt) override {
        switch (tt) {
            case litehtml::text_transform_uppercase:
                std::ranges::transform(text, text.begin(),
                                       [](unsigned char c) { return std::toupper(c); });
                break;
            case litehtml::text_transform_lowercase:
                std::ranges::transform(text, text.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                break;
            default:
                break;
        }
    }

    void import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl) override {
        std::string abs = makeAbsolute(url, baseurl);
        std::string data;
        if (httpGet(abs, data)) {
            text = data;
            baseurl = abs; // resolve url()s inside the stylesheet relative to its location
        }
    }

    void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &bdr_radius) override {
        auto canvas = reinterpret_cast<SkCanvas *>(get_hdc());
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(pos.x, pos.y, pos.width, pos.height));
    }

    void del_clip() override {
        auto canvas = reinterpret_cast<SkCanvas *>(get_hdc());
        canvas->restore();
    }

    void get_viewport(litehtml::position &viewport) const override {
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = width_;
        viewport.height = height_;
    }

    litehtml::element::ptr create_element(const char *tag_name, const litehtml::string_map &attributes,
                                          const std::shared_ptr<litehtml::document> &doc) override {
        return std::make_shared<litehtml::html_tag>(doc);
    }

    void get_media_features(litehtml::media_features &media) const override {
        media.type = litehtml::media_type_screen;
        media.width = width_;
        media.height = height_;
        media.device_width = width_;
        media.device_height = height_;
        media.color = 8;
        media.monochrome = 0;
        media.color_index = 0;
        media.resolution = 96;
    }

    void get_language(litehtml::string &language, litehtml::string &culture) const override {
        language = "en";
        culture = "en-US";
    }

    virtual litehtml::uint_ptr get_hdc() const = 0;
};

class AppDocumentContainer : public DocumentContainer {
    SkCanvas *canvas_ = nullptr;

public:
    void setCanvas(SkCanvas *canvas) {
        canvas_ = canvas;
    }

    litehtml::uint_ptr get_hdc() const override {
        return reinterpret_cast<litehtml::uint_ptr>(canvas_);
    }
};

struct App {
    VkAllocationCallbacks *allocator = nullptr;

    vkb::Instance instance_;
    vkb::InstanceDispatchTable vkInstance_;
    SDL_Window *window_;
    VkSurfaceKHR surface_;
    vkb::PhysicalDevice physicalDevice_;
    vkb::Device device_;
    vkb::DispatchTable vk_;
    VkQueue graphicsQueue_;
    VkQueue presentQueue_;
    uint32_t graphicsQueueIndex_;
    uint32_t presentQueueIndex_;
    vkb::Swapchain swapchain_;
    std::vector<VkImage> swapchainImages_;
    std::unique_ptr<skgpu::graphite::Context> context_;
    std::unique_ptr<skgpu::graphite::Recorder> recorder_;

    AppDocumentContainer container;
    std::shared_ptr<litehtml::document> doc;

    struct Frame {
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkSemaphore signal = VK_NULL_HANDLE;
        VkFence presentFence = VK_NULL_HANDLE;
        bool presentPending = false;
    };

    std::vector<Frame> frames;
    std::vector<sk_sp<SkSurface> > surfaces;
    int frameIdx = 0;
    uint32_t currentImage = 0;

    App() = default;

    auto Init(std::span<char *> args) -> SDL_AppResult {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        chkSDL(SDL_SetAppMetadata("Codotaku Web", "0.0.1", "com.codotaku.web"));
        chkSDL(SDL_Init(SDL_INIT_VIDEO));
        chkSDL(SDL_Vulkan_LoadLibrary(nullptr));
        auto vkProc = SDL_Vulkan_GetVkGetInstanceProcAddr();
        chkSDL(vkProc);
        auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkProc);
        vkb::InstanceBuilder instanceBuilder(vkGetInstanceProcAddr);

        uint32_t instanceExtensionCount;
        std::span instanceExtensions{SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount), instanceExtensionCount};

        instanceBuilder
                .request_validation_layers()
                .require_api_version(VK_API_VERSION_1_4)
                .use_default_debug_messenger()
                .set_allocation_callbacks(allocator);

        for (auto instanceExtension: instanceExtensions)
            instanceBuilder.enable_extension(instanceExtension);

        instanceBuilder.enable_extension("VK_KHR_get_surface_capabilities2");
        instanceBuilder.enable_extension("VK_EXT_surface_maintenance1");

        instance_ = chkVkb(instanceBuilder.build());
        vkInstance_ = instance_.make_table();

        window_ = SDL_CreateWindow("Codotaku Web", 800, 600,
                                   SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
        chkSDL(window_);

        chkSDL(SDL_Vulkan_CreateSurface(window_, instance_.instance, allocator, &surface_));

        vkb::PhysicalDeviceSelector physicalDeviceSelector(instance_, surface_);
        physicalDeviceSelector.add_required_extension("VK_EXT_swapchain_maintenance1");
        physicalDeviceSelector.add_required_extension("VK_EXT_present_timing");
        physicalDeviceSelector.add_required_extension("VK_KHR_present_id2");
        physicalDeviceSelector.add_required_extension("VK_KHR_calibrated_timestamps");
        physicalDeviceSelector.add_required_extension("VK_KHR_present_mode_fifo_latest_ready");

        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maint1{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
            .swapchainMaintenance1 = VK_TRUE,
        };
        physicalDeviceSelector.add_required_extension_features(maint1);
        VkPhysicalDevicePresentTimingFeaturesEXT presentTiming{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
            .presentTiming = VK_TRUE,
        };
        physicalDeviceSelector.add_required_extension_features(presentTiming);
        VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR fifoLr{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
            .presentModeFifoLatestReady = VK_TRUE,
        };
        physicalDeviceSelector.add_required_extension_features(fifoLr);

        auto physicalDeviceResult = physicalDeviceSelector.select();
        physicalDevice_ = chkVkb(physicalDeviceResult);

        vkb::DeviceBuilder deviceBuilder(physicalDevice_);
        auto deviceResult = deviceBuilder.build();
        device_ = chkVkb(deviceResult);

        auto [graphicsQueue, graphicsQueueIndex] = chkVkb(device_.get_queue_and_index(vkb::QueueType::graphics));
        auto [presentQueue, presentQueueIndex] = chkVkb(device_.get_queue_and_index(vkb::QueueType::present));
        graphicsQueue_ = graphicsQueue;
        presentQueue_ = presentQueue;
        graphicsQueueIndex_ = graphicsQueueIndex;
        presentQueueIndex_ = presentQueueIndex;

        vkb::SwapchainBuilder swapchainBuilder(device_);
        swapchainBuilder
                .set_create_flags(VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR)
                .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        swapchain_ = chkVkb(swapchainBuilder.build());
        swapchainImages_ = chkVkb(swapchain_.get_images());

        auto getProc = [gipa = instance_.fp_vkGetInstanceProcAddr,
                    gdpa = device_.fp_vkGetDeviceProcAddr]
        (const char *name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
            if (device != VK_NULL_HANDLE) return gdpa(device, name);
            return gipa(instance, name);
        };

        skgpu::VulkanBackendContext vulkanBackendContext{
            .fInstance = instance_.instance,
            .fPhysicalDevice = physicalDevice_.physical_device,
            .fDevice = device_.device,
            .fQueue = graphicsQueue_,
            .fGraphicsQueueIndex = graphicsQueueIndex_,
            .fMaxAPIVersion = VK_API_VERSION_1_4,
            .fVkExtensions = nullptr,
            .fDeviceFeatures = nullptr,
            .fDeviceFeatures2 = nullptr,
            .fGetProc = getProc,
            .fProtectedContext = skgpu::Protected::kNo,
            .fDeviceLostContext = {},
            .fDeviceLostProc = {}
        };
        vulkanBackendContext.fMemoryAllocator = skgpu::VulkanMemoryAllocators::Make(
            vulkanBackendContext, skgpu::ThreadSafe::kNo);

        vk_ = device_.make_table();

        frames.resize(MAX_FRAMES);
        for (auto &f: frames) {
            VkSemaphoreCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            chk(vk_.createSemaphore(&sci, allocator, &f.acquire));
            chk(vk_.createSemaphore(&sci, allocator, &f.signal));
            VkFenceCreateInfo pfci{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            };
            chk(vk_.createFence(&pfci, allocator, &f.presentFence));
        }

        skgpu::graphite::ContextOptions contextOptions;
        context_ = skgpu::graphite::ContextFactory::MakeVulkan(vulkanBackendContext, contextOptions);
        if (!context_) {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to create graphite context");
            std::exit(1);
        }

        recorder_ = context_->makeRecorder();

        rebuildSurfaces();

        std::string url = args.size() > 1 ? args[1] : "https://example.com";
        container.set_page_url(url);

        std::string html;
        if (!httpGet(url, html) || html.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "Failed to load %s", url.c_str());
            html = "<html><body><h1>Failed to load page</h1></body></html>";
        }

        doc = litehtml::document::createFromString(html.c_str(), &container);
        container.set_size(swapchain_.extent.width, swapchain_.extent.height);

        chkSDL(SDL_ShowWindow(window_));
        return SDL_APP_CONTINUE;
    }

    auto Iterate() -> SDL_AppResult {
        if (!context_) return SDL_APP_CONTINUE;

        auto &frame = frames[frameIdx];

        if (frame.presentPending) {
            chk(vk_.waitForFences(1, &frame.presentFence, VK_TRUE, UINT64_MAX));
            chk(vk_.resetFences(1, &frame.presentFence));
            frame.presentPending = false;
        }

        uint32_t nextImage;
        auto acqRes = vk_.acquireNextImageKHR(
            swapchain_.swapchain, UINT64_MAX, frame.acquire, VK_NULL_HANDLE, &nextImage);
        if (acqRes == VK_ERROR_OUT_OF_DATE_KHR) {
            rebuildSwapchain();
            return SDL_APP_CONTINUE;
        }
        chk(acqRes);
        currentImage = nextImage;

        auto canvas = surfaces[currentImage]->getCanvas();

        container.setCanvas(canvas);
        container.set_size(swapchain_.extent.width, swapchain_.extent.height);
        doc->render(swapchain_.extent.width);

        SkPaint paint;
        paint.setColor(SkColors::kWhite);
        canvas->drawPaint(paint);

        litehtml::position clip(0, 0, swapchain_.extent.width, swapchain_.extent.height);
        doc->draw(reinterpret_cast<litehtml::uint_ptr>(canvas), 0, 0, &clip);

        auto recording = recorder_->snap();
        if (!recording) {
            SDL_LogError(SDL_LOG_CATEGORY_ERROR, "snap() failed");
            return SDL_APP_CONTINUE;
        }

        skgpu::graphite::InsertRecordingInfo info{};
        info.fRecording = recording.get();
        info.fTargetSurface = surfaces[currentImage].get();

        auto presentState = skgpu::MutableTextureStates::MakeVulkan(
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, presentQueueIndex_);
        info.fTargetTextureState = &presentState;

        info.fNumWaitSemaphores = 1;
        auto backendAcquire = skgpu::graphite::BackendSemaphores::MakeVulkan(frame.acquire);
        info.fWaitSemaphores = &backendAcquire;

        info.fNumSignalSemaphores = 1;
        auto backendSignal = skgpu::graphite::BackendSemaphores::MakeVulkan(frame.signal);
        info.fSignalSemaphores = &backendSignal;

        context_->insertRecording(info);
        context_->submit(skgpu::graphite::SyncToCpu::kNo);

        VkSwapchainPresentFenceInfoKHR presentFenceInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
            .swapchainCount = 1,
            .pFences = &frame.presentFence,
        };
        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = &presentFenceInfo,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &frame.signal,
            .swapchainCount = 1,
            .pSwapchains = &swapchain_.swapchain,
            .pImageIndices = &currentImage,
        };
        auto presRes = vk_.queuePresentKHR(presentQueue_, &presentInfo);
        if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) {
            chk(vk_.resetFences(1, &frame.presentFence));
            rebuildSwapchain();
        } else if (presRes == VK_ERROR_OUT_OF_HOST_MEMORY || presRes == VK_ERROR_OUT_OF_DEVICE_MEMORY || presRes ==
                   VK_ERROR_DEVICE_LOST) {
            chk(vk_.resetFences(1, &frame.presentFence));
            chk(presRes);
        } else {
            chk(presRes);
            frame.presentPending = true;
        }

        frameIdx = (frameIdx + 1) % MAX_FRAMES;
        return SDL_APP_CONTINUE;
    }

    auto Event(SDL_Event *event) -> SDL_AppResult {
        switch (event->type) {
            case SDL_EVENT_QUIT:
                return SDL_APP_SUCCESS;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_EXPOSED:
                rebuildSwapchain();
                return SDL_APP_CONTINUE;
            default:
                return SDL_APP_CONTINUE;
        }
    }

    auto Quit(SDL_AppResult result) -> void {
        if (!context_) return;
        surfaces.clear();
        context_->submit(skgpu::graphite::SyncToCpu::kYes);
        for (auto &f: frames) {
            if (f.presentPending) {
                chk(vk_.waitForFences(1, &f.presentFence, VK_TRUE, UINT64_MAX));
            }
            if (f.acquire != VK_NULL_HANDLE) vk_.destroySemaphore(f.acquire, allocator);
            if (f.signal != VK_NULL_HANDLE) vk_.destroySemaphore(f.signal, allocator);
            if (f.presentFence != VK_NULL_HANDLE) vk_.destroyFence(f.presentFence, allocator);
        }
        frames.clear();
        vkb::destroy_swapchain(swapchain_);
        recorder_.reset();
        context_.reset();
        vkb::destroy_device(device_);
        vkInstance_.destroySurfaceKHR(surface_, allocator);
        SDL_DestroyWindow(window_);
        vkb::destroy_instance(instance_);
        curl_global_cleanup();
    }

private:
    void rebuildSwapchain() {
        surfaces.clear();

        // Wait for any pending present fences instead of a full device wait
        for (auto &f: frames) {
            if (f.presentPending) {
                chk(vk_.waitForFences(1, &f.presentFence, VK_TRUE, UINT64_MAX));
                chk(vk_.resetFences(1, &f.presentFence));
                f.presentPending = false;
            }
        }

        VkSwapchainKHR old = swapchain_.swapchain;

        vkb::SwapchainBuilder swapchainBuilder(device_);
        swapchainBuilder
                .set_old_swapchain(old)
                .set_create_flags(VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_KHR)
                .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        auto newSwapchain = chkVkb(swapchainBuilder.build());
        vk_.destroySwapchainKHR(old, allocator);
        swapchain_ = newSwapchain;

        swapchainImages_ = chkVkb(swapchain_.get_images());
        rebuildSurfaces();
    }

    void rebuildSurfaces() {
        surfaces.clear();
        for (auto img: swapchainImages_) {
            skgpu::graphite::VulkanTextureInfo texInfo(
                VK_SAMPLE_COUNT_1_BIT,
                skgpu::Mipmapped::kNo,
                0,
                swapchain_.image_format,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                VK_SHARING_MODE_EXCLUSIVE,
                VK_IMAGE_ASPECT_COLOR_BIT,
                {});
            skgpu::VulkanAlloc zeroAlloc{};
            auto backendTex = skgpu::graphite::BackendTextures::MakeVulkan(
                {static_cast<int>(swapchain_.extent.width), static_cast<int>(swapchain_.extent.height)},
                texInfo, VK_IMAGE_LAYOUT_UNDEFINED, VK_QUEUE_FAMILY_IGNORED,
                img, zeroAlloc);
            auto surf = SkSurfaces::WrapBackendTexture(
                recorder_.get(), backendTex, SkColorSpace::MakeSRGB(), nullptr);
            if (!surf) {
                surfaces.clear();
                throw std::runtime_error{"WrapBackendTexture failed for swapchain image"};
            }
            surfaces.push_back(std::move(surf));
        }
    }
};

auto SDL_AppInit(void **appstate, int argc, char **argv) -> SDL_AppResult {
    auto app = std::make_unique<App>();
    auto result = app->Init({argv, static_cast<size_t>(argc)});
    *appstate = app.release();
    return result;
}

auto SDL_AppIterate(void *appstate) -> SDL_AppResult {
    auto app = static_cast<App *>(appstate);
    return app->Iterate();
}

auto SDL_AppEvent(void *appstate, SDL_Event *event) -> SDL_AppResult {
    auto app = static_cast<App *>(appstate);
    return app->Event(event);
}

auto SDL_AppQuit(void *appstate, SDL_AppResult result) -> void {
    auto app = static_cast<App *>(appstate);
    app->Quit(result);
}
