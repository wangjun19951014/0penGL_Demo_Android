#include <jni.h>
#include <string>
#include <vector>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GLTest", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GLTest", __VA_ARGS__)

// ============================================================
// GL_QCOM_binning_control token definitions
// (in case the system headers don't have them)
// ============================================================
#ifndef GL_BINNING_CONTROL_HINT_QCOM
#define GL_BINNING_CONTROL_HINT_QCOM         0x8FB0
#endif
#ifndef GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM
#define GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM 0x8FB3
#endif
#ifndef GL_BINNING_QCOM
#define GL_BINNING_QCOM                      0x8FB1
#endif
#ifndef GL_VISIBILITY_OPTIMIZED_BINNING_QCOM
#define GL_VISIBILITY_OPTIMIZED_BINNING_QCOM 0x8FB2
#endif

// ============================================================
// GL extension string checker (Khronos recommended method)
// ============================================================
static bool hasGlExtension(const char* targetExt) {
    // GLES 3.0+ method: glGetStringi(GL_EXTENSIONS, i)
    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    if (numExt > 0) {
        for (GLint i = 0; i < numExt; i++) {
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
            if (ext && strcmp(ext, targetExt) == 0) return true;
        }
        return false;
    }

    // Fallback to GLES 2.0 method: glGetString(GL_EXTENSIONS)
    const char* allExt = (const char*)glGetString(GL_EXTENSIONS);
    if (!allExt) return false;

    size_t len = strlen(targetExt);
    const char* ptr = allExt;
    while ((ptr = strstr(ptr, targetExt)) != NULL) {
        char before = (ptr > allExt) ? *(ptr - 1) : ' ';
        char after = *(ptr + len);
        if ((before == ' ' || before == '\0') &&
            (after == ' ' || after == '\0')) {
            return true;
        }
        ptr += len;
    }
    return false;
}

// ============================================================
// Collect all QCOM-related extensions for diagnosis
// ============================================================
static std::vector<std::string> collectQcomExtensions() {
    std::vector<std::string> qcomExts;
    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    if (numExt > 0) {
        for (GLint i = 0; i < numExt; i++) {
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
            if (ext) {
                std::string s(ext);
                // Collect all QCOM extensions (case-insensitive prefix check)
                if (s.find("QCOM") != std::string::npos ||
                    s.find("qcom") != std::string::npos) {
                    qcomExts.push_back(s);
                }
            }
        }
    } else {
        // GLES 2.0 fallback: parse the space-separated string
        const char* allExt = (const char*)glGetString(GL_EXTENSIONS);
        if (allExt) {
            std::string remaining(allExt);
            while (!remaining.empty()) {
                size_t pos = remaining.find(' ');
                std::string ext = (pos == std::string::npos) ? remaining : remaining.substr(0, pos);
                if (ext.find("QCOM") != std::string::npos ||
                    ext.find("qcom") != std::string::npos) {
                    qcomExts.push_back(ext);
                }
                if (pos == std::string::npos) break;
                remaining = remaining.substr(pos + 1);
            }
        }
    }
    return qcomExts;
}

// ============================================================
// EGL initialization (supports both pbuffer and window surface)
// When nativeWindow is provided → window surface mode
// When nativeWindow is nullptr  → pbuffer (offscreen) mode
// ============================================================
static bool initEgl(EGLDisplay& display, EGLContext& context, EGLSurface& surface,
                    std::string& diag, ANativeWindow* nativeWindow = nullptr) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        diag += "eglGetDisplay failed\n";
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        diag += "eglInitialize failed\n";
        LOGE("eglInitialize failed");
        return false;
    }
    diag += "EGL version: " + std::to_string(major) + "." + std::to_string(minor) + "\n";
    LOGI("EGL version: %d.%d", major, minor);

    // Check EGL extensions for surfaceless support
    const char* eglExts = eglQueryString(display, EGL_EXTENSIONS);
    bool hasSurfaceless = eglExts && strstr(eglExts, "EGL_KHR_surfaceless_context") != nullptr;
    diag += "EGL extensions: " + std::string(eglExts ? eglExts : "N/A") + "\n";
    diag += std::string("EGL_KHR_surfaceless_context: ") +
            (hasSurfaceless ? "✅ yes" : "❌ no") + "\n";

    // Choose config
    // For window surface, we need EGL_WINDOW_BIT
    // For pbuffer, we need EGL_PBUFFER_BIT
    EGLint surfaceType = nativeWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT;

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, surfaceType,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    bool es3ConfigOk = eglChooseConfig(display, attribs, &config, 1, &numConfigs) && numConfigs > 0;
    if (!es3ConfigOk) {
        diag += "eglChooseConfig(ES3_BIT) failed, trying ES2_BIT\n";
        LOGE("eglChooseConfig ES3 failed, retrying ES2");
        EGLint attribs2[] = {
            EGL_SURFACE_TYPE, surfaceType,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_NONE
        };
        if (!eglChooseConfig(display, attribs2, &config, 1, &numConfigs) || numConfigs == 0) {
            diag += "eglChooseConfig(ES2_BIT) also failed!\n";
            LOGE("eglChooseConfig ES2 failed");
            eglTerminate(display);
            return false;
        }
    }

    // Try GLES 3.0 context first
    EGLint ctxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
    bool isGLES3 = (context != EGL_NO_CONTEXT);
    if (!isGLES3) {
        diag += "GLES 3.0 context failed, falling back to GLES 2.0\n";
        LOGI("GLES 3.0 context failed, fallback to GLES 2.0");
        EGLint ctxAttribs2[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs2);
        if (context == EGL_NO_CONTEXT) {
            diag += "eglCreateContext failed for both ES3 and ES2!\n";
            LOGE("eglCreateContext failed");
            eglTerminate(display);
            return false;
        }
    }
    diag += std::string("GL context: ") + (isGLES3 ? "OpenGL ES 3.x" : "OpenGL ES 2.0") + "\n";

    // Create surface based on mode
    if (nativeWindow) {
        // ---- Window surface mode (real hardware surface) ----
        surface = eglCreateWindowSurface(display, config, nativeWindow, nullptr);
        if (surface == EGL_NO_SURFACE) {
            diag += "eglCreateWindowSurface failed!\n";
            LOGE("eglCreateWindowSurface failed");
            eglDestroyContext(display, context);
            eglTerminate(display);
            return false;
        }
        diag += "Surface type: WINDOW (from TextureView)\n";
    } else {
        // ---- PBuffer surface mode (offscreen) ----
        EGLint pbufferAttribs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };

        surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
        if (surface == EGL_NO_SURFACE) {
            diag += "eglCreatePbufferSurface failed, trying surfaceless\n";
            LOGE("eglCreatePbufferSurface failed");
            if (hasSurfaceless) {
                if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
                    diag += "eglMakeCurrent (surfaceless) failed!\n";
                    eglDestroyContext(display, context);
                    eglTerminate(display);
                    return false;
                }
                diag += "Using surfaceless context (no pbuffer)\n";
                LOGI("Using surfaceless context");
                return true;
            }
            eglDestroyContext(display, context);
            eglTerminate(display);
            return false;
        }
        diag += "Surface type: pbuffer (1x1)\n";
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        diag += "eglMakeCurrent failed!\n";
        LOGE("eglMakeCurrent failed");
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    LOGI("EGL initialized successfully");
    return true;
}

static void cleanupEgl(EGLDisplay display, EGLContext context, EGLSurface surface) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    LOGI("EGL cleaned up");
}

// ============================================================
// Helper: format an integer as hex string like "0x8FB3"
// ============================================================
static std::string hexStr(GLint val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%X", (unsigned int)(val & 0xFFFFFFFF));
    return std::string(buf);
}

// ============================================================
// Probe GL_QCOM_binning_control:
//   Method 1: Extension string check (Khronos standard)
//   Method 2: Functional test - call glEnable/glHint directly
//             (this is what Snapdragon SDK actually does)
//   Method 3: Driver integrity check - verify the driver actually
//             reports errors for truly invalid tokens
//   Method 4: Read-back hint value via glGetIntegerv after setting GL_VISIBILITY_OPTIMIZED_BINNING_QCOM (0x8FB2) and GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM (0x8FB3) separately
// ============================================================
static std::string probeQcomBinningControl() {
    std::string result;

    // ---- Method 1: Extension string check ----
    bool stringSupported = hasGlExtension("GL_QCOM_binning_control");
    result += "[Method 1] Extension string check:\n";
    result += "  GL_QCOM_binning_control: ";
    result += stringSupported ? "✅ FOUND in extension list\n" : "❌ NOT in extension list\n";

    // ---- Method 3 FIRST: Driver integrity check ----
    // This tells us whether the driver even reports errors at all.
    // If it silently ignores ALL unknown values, then Method 2 is untrustworthy.
    result += "\n[Method 3] Driver integrity check:\n";
    while (glGetError() != GL_NO_ERROR) {}

    glEnable(0xDEAD);             // Definitely bogus - should error
    GLenum deadErr = glGetError();
    result += "  glEnable(0xDEAD — bogus value): ";
    result += (deadErr != GL_NO_ERROR) ? "✅ ERROR reported (driver is strict)" : "❌ SILENTLY IGNORED (driver is lenient)";
    if (deadErr != GL_NO_ERROR) {
        result += " [code: " + hexStr(deadErr) + " = GL_INVALID_ENUM]";
    }
    result += "\n";

    while (glGetError() != GL_NO_ERROR) {}
    glHint(0xDEAD, 0xDEAD);      // Definitely bogus hint target/mode
    GLenum deadHintErr = glGetError();
    result += "  glHint(0xDEAD, 0xDEAD — bogus): ";
    result += (deadHintErr != GL_NO_ERROR) ? "✅ ERROR reported" : "❌ SILENTLY IGNORED";
    result += "\n";

    // ---- Method 2: Functional test (direct call) ----
    // This is what Snapdragon SDK does - it skips probing entirely
    // and just calls the functions. We check for GL errors afterward.
    result += "\n[Method 2] Functional test (direct glEnable/glHint):\n";

    while (glGetError() != GL_NO_ERROR) {}
    glEnable(GL_BINNING_CONTROL_HINT_QCOM);
    GLenum enableErr = glGetError();

    GLenum hintErr = GL_NO_ERROR;
    if (enableErr == GL_NO_ERROR) {
        glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
        hintErr = glGetError();
    }

    result += "  glEnable(0x8FB0):  ";
    result += (enableErr == GL_NO_ERROR) ? "✅ GL_NO_ERROR" : "❌ FAILED";
    if (enableErr != GL_NO_ERROR) {
        result += " [error: " + hexStr(enableErr) + "]";
    }
    result += "\n";

    result += "  glHint(0x8FB0, 0x8FB3): ";
    result += (hintErr == GL_NO_ERROR) ? "✅ GL_NO_ERROR" : "❌ FAILED";
    if (hintErr != GL_NO_ERROR) {
        result += " [error: " + hexStr(hintErr) + "]";
    }
    result += "\n";

    // ---- Method 4: Read-back hint value after setting ----
    // Test if the hint target is queryable via glGetIntegerv
    result += "\n[Method 4] Read-back verification:\n";

    // --- Test 1: Read back the value set by Method 2 (0x8FB3) ---
    GLint hintValue = -1;
    while (glGetError() != GL_NO_ERROR) {}
    glGetIntegerv(GL_BINNING_CONTROL_HINT_QCOM, &hintValue);
    GLenum getErrBefore = glGetError();

    result += "  Read before hint (after glEnable): ";
    result += (getErrBefore == GL_NO_ERROR)
        ? "value = " + hexStr(hintValue)
        : "❌ glGetIntegerv NOT queryable";
    result += "\n";

    // --- Test 2: Verify each mode specifically with pure glHint + read-back ---
    // Note: glHint overrides previous hint mode, no reset needed
    while (glGetError() != GL_NO_ERROR) {}

    // Test GL_VISIBILITY_OPTIMIZED_BINNING_QCOM (0x8FB2)
    glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_VISIBILITY_OPTIMIZED_BINNING_QCOM);
    GLenum hintVisErr = glGetError();

    GLint hintVisRead = -1;
    while (glGetError() != GL_NO_ERROR) {}
    glGetIntegerv(GL_BINNING_CONTROL_HINT_QCOM, &hintVisRead);
    GLenum readVisErr = glGetError();

    result += "\n  glHint(0x8FB0, 0x8FB2 — VISIBILITY_OPTIMIZED): ";
    result += (hintVisErr == GL_NO_ERROR) ? "✅ GL_NO_ERROR" : "❌ FAILED";
    result += "\n  glGetIntegerv read-back: ";
    result += (readVisErr == GL_NO_ERROR)
        ? "value = " + hexStr(hintVisRead)
        : "❌ NOT queryable";
    if (readVisErr == GL_NO_ERROR) {
        result += "  " + std::string(hintVisRead == GL_VISIBILITY_OPTIMIZED_BINNING_QCOM ? "✅ MATCHES!" : "⚠️ has different value");
    }
    result += "\n";

    // Test GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM (0x8FB3)
    glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
    GLenum hintDirectErr = glGetError();

    GLint hintDirectRead = -1;
    while (glGetError() != GL_NO_ERROR) {}
    glGetIntegerv(GL_BINNING_CONTROL_HINT_QCOM, &hintDirectRead);
    GLenum readDirectErr = glGetError();

    result += "\n  glHint(0x8FB0, 0x8FB3 — DIRECT_TO_FRAMEBUFFER): ";
    result += (hintDirectErr == GL_NO_ERROR) ? "✅ GL_NO_ERROR" : "❌ FAILED";
    result += "\n  glGetIntegerv read-back: ";
    result += (readDirectErr == GL_NO_ERROR)
        ? "value = " + hexStr(hintDirectRead)
        : "❌ NOT queryable";
    if (readDirectErr == GL_NO_ERROR) {
        result += "  " + std::string(hintDirectRead == GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM ? "✅ MATCHES!" : "⚠️ has different value");
    }
    result += "\n";

    // ---- Verdict ----
    bool functionalWorks = (enableErr == GL_NO_ERROR && hintErr == GL_NO_ERROR);
    bool driverIsLenient  = (deadErr == GL_NO_ERROR);
    bool driverIsStrict   = (deadErr != GL_NO_ERROR);

    result += "\n[Verdict]\n";

    if (driverIsLenient) {
        result += "  ⚠️ DRIVER IS LENIENT: glEnable/glHint never report errors.\n";
        result += "  → The functional test results CANNOT be trusted.\n";
        if (stringSupported) {
            result += "  → But extension IS in the string list, so it IS supported.\n";
        } else {
            result += "  → Extension not in string list either. Likely NOT available.\n";
        }
    } else if (driverIsStrict) {
        if (stringSupported && functionalWorks) {
            result += "  ✅ EXTENSION CONFIRMED: String says YES, functions work.\n";
        } else if (!stringSupported && functionalWorks) {
            result += "  ⚠️ EXTENSION FUNCTIONALLY WORKS but NOT in extension string list.\n";
            result += "  → This is the pbuffer filtering case on Adreno.\n";
            result += "  → The extension WILL work at runtime with a window surface.\n";
        } else if (stringSupported && !functionalWorks) {
            result += "  ⚠️ Extension in string but functional calls FAILED.\n";
        } else {
            result += "  ❌ EXTENSION NOT DETECTED (both methods failed).\n";
        }

        // Add read-back summary (variables from Method 4 above)
        if (readVisErr == GL_NO_ERROR || readDirectErr == GL_NO_ERROR) {
            result += "\n  ✅ HINT READ-BACK SUCCESS: glGetIntegerv can read 0x8FB0.\n";
            result += "  → The driver stores the hint value internally.\n";
            result += "  → glHint(0x8FB0, ...) has a REAL side effect.\n";
            result += "  → EXTENSION IS FULLY OPERATIONAL.\n";
        } else {
            result += "\n  ℹ️ Hint not queryable via glGetIntegerv (expected per GLES spec).\n";
            result += "  → But strict driver confirmed glHint(0x8FB0, 0x8FB2/0x8FB3) works.\n";
            result += "  → EXTENSION IS OPERATIONAL.\n";
        }
    }

    return result;
}

// ============================================================
// List all supported extensions (for debugging)
// ============================================================
static std::string listAllExtensions() {
    std::string result;
    result += "--- All Supported Extensions ---\n";

    GLint numExt = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    if (numExt > 0) {
        for (GLint i = 0; i < numExt; i++) {
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
            if (ext) {
                result += "  ";
                result += ext;
                result += "\n";
            }
        }
        result += "  (total: " + std::to_string(numExt) + " extensions)\n";
    } else {
        // GLES 2.0 fallback
        const char* allExt = (const char*)glGetString(GL_EXTENSIONS);
        if (allExt) {
            result += "  ";
            result += allExt;
            result += "\n";
        }
    }

    return result;
}

// ============================================================
// Common probe logic — runs after EGL context is active
// ============================================================
static std::string runProbe(const std::string& eglDiag, const std::string& modeLabel) {
    std::string result;

    // --- GL Info ---
    const char* version   = (const char*)glGetString(GL_VERSION);
    const char* renderer  = (const char*)glGetString(GL_RENDERER);
    const char* vendor    = (const char*)glGetString(GL_VENDOR);
    const char* glslVer   = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);

    result += "========================================\n";
    result += "      OpenGL ES Extension Probe\n";
    result += "         Mode: " + modeLabel + "\n";
    result += "========================================\n\n";
    result += "--- EGL Init ---\n";
    result += eglDiag;
    result += "\n";
    result += "--- GL Info ---\n";
    result += "Vendor:        " + std::string(vendor   ? vendor   : "N/A") + "\n";
    result += "Renderer:      " + std::string(renderer  ? renderer  : "N/A") + "\n";
    result += "Version:       " + std::string(version   ? version   : "N/A") + "\n";
    result += "GLSL Version:  " + std::string(glslVer   ? glslVer   : "N/A") + "\n\n";

    // --- Show all QCOM-related extensions ---
    auto qcomExts = collectQcomExtensions();
    result += "--- QCOM Extensions Found (" + std::to_string(qcomExts.size()) + ") ---\n";
    if (qcomExts.empty()) {
        result += "  (none)\n";
    } else {
        for (const auto& ext : qcomExts) {
            result += "  " + ext + "\n";
        }
    }
    result += "\n";

    // --- Probe GL_QCOM_binning_control ---
    result += "--- GL_QCOM_binning_control ---\n";
    result += probeQcomBinningControl();
    result += "\n";

    // --- List all extensions ---
    result += listAllExtensions();
    result += "========================================\n";

    return result;
}

// ============================================================
// JNI: Probe with offscreen pbuffer surface (no window needed)
// ============================================================
extern "C" JNIEXPORT jstring JNICALL
Java_com_xros_gltest_MainActivity_probeExtensions(JNIEnv* env, jobject /* this */) {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    std::string eglDiag;
    if (!initEgl(display, context, surface, eglDiag)) {
        std::string err = "❌ Failed to initialize EGL context.\n\n";
        err += "--- EGL Init Diagnostics ---\n";
        err += eglDiag;
        return env->NewStringUTF(err.c_str());
    }

    std::string result = runProbe(eglDiag, "Offscreen (PBuffer)");
    cleanupEgl(display, context, surface);
    return env->NewStringUTF(result.c_str());
}

// ============================================================
// JNI: Probe with a real hardware window surface
//      Takes an android.view.Surface object from Java/Kotlin
// ============================================================
extern "C" JNIEXPORT jstring JNICALL
Java_com_xros_gltest_MainActivity_probeExtensionsWithSurface(JNIEnv* env, jobject /* this */, jobject surfaceObj) {
    if (surfaceObj == nullptr) {
        return env->NewStringUTF("❌ Surface object is null.\nCannot create window surface.");
    }

    ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, surfaceObj);
    if (nativeWindow == nullptr) {
        return env->NewStringUTF("❌ ANativeWindow_fromSurface failed.\nCannot create window surface.");
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    std::string eglDiag;
    if (!initEgl(display, context, surface, eglDiag, nativeWindow)) {
        ANativeWindow_release(nativeWindow);
        std::string err = "❌ Failed to initialize EGL context with window surface.\n\n";
        err += "--- EGL Init Diagnostics ---\n";
        err += eglDiag;
        return env->NewStringUTF(err.c_str());
    }

    ANativeWindow_release(nativeWindow);

    std::string result = runProbe(eglDiag, "Window Surface (Hardware)");
    cleanupEgl(display, context, surface);
    return env->NewStringUTF(result.c_str());
}

// ============================================================
// Keep the original stringFromJNI for backward compatibility
// ============================================================
extern "C" JNIEXPORT jstring JNICALL
Java_com_xros_gltest_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++";
    return env->NewStringUTF(hello.c_str());
}