# OpenGL ES 扩展探测技术分析

> 本文档以 `GL_QCOM_binning_control` 扩展为例，深入分析 OpenGL ES 扩展探测的各种方法及其可靠性，并给出最佳实践。

---

## 目录

1. [背景：GL_QCOM_binning_control 扩展](#1-背景gl_qcom_binning_control-扩展)
2. [探测方法一：glError 探测法](#2-探测方法一glerror-探测法)
3. [探测方法二：扩展字符串检查法](#3-探测方法二扩展字符串检查法)
4. [探测方法三：函数指针检查法](#4-探测方法三函数指针检查法)
5. [方法对比与推荐](#5-方法对比与推荐)
6. [Snapdragon SDK 的做法](#6-snapdragon-sdk-的做法)

---

## 1. 背景：GL_QCOM_binning_control 扩展

### 1.1 扩展概述

**名称字符串：** `GL_QCOM_binning_control`

该扩展用于控制高通 Adreno GPU 的 tile-based 渲染行为，提供两种模式：

| Hint Mode | 值 | 含义 |
|:---|:---:|:---|
| `GL_BINNING_QCOM` | 0x8FB1 | 默认 tiling/binning 模式，渲染到 tile buffer 再写回 framebuffer |
| `GL_VISIBILITY_OPTIMIZED_BINNING_QCOM` | 0x8FB2 | 带可见性优化的 binning |
| **`GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM`** | **0x8FB3** | **绕过 tile buffer，直接写 framebuffer** |

### 1.2 涉及的关键 Token

```cpp
// 标准头文件中定义的 token
#define GL_BINNING_CONTROL_HINT_QCOM         0x8FB0   // glHint() 的 target
#define GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM 0x8FB3   // glHint() 的 mode
```

### 1.3 正确启用方式

```cpp
// 假设扩展已确认存在
glEnable(GL_BINNING_CONTROL_HINT_QCOM);
glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
```

---

## 2. 探测方法一：glError 探测法

### 2.1 代码示例

```cpp
bool probeDirectToFramebufferViaErrors() {
    GLint binningHint = -1;
    glGetError();  // 清除之前的 GL 错误

    // 步骤 1: 尝试查询 hint 值
    glGetIntegerv(GL_BINNING_CONTROL_HINT_QCOM, &binningHint);
    GLenum queryErr = glGetError();

    // 步骤 2: 尝试设置 hint
    glGetError();  // 清除
    glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
    GLenum hintErr = glGetError();

    // 步骤 3: 再次查询
    GLint binningHintAfter = -1;
    glGetError();  // 清除
    glGetIntegerv(GL_BINNING_CONTROL_HINT_QCOM, &binningHintAfter);
    GLenum queryAfterErr = glGetError();

    // 判断：两次查询和一次设置都无错则判为支持
    return (queryErr == GL_NO_ERROR) &&
           (hintErr == GL_NO_ERROR) &&
           (queryAfterErr == GL_NO_ERROR);
}
```

### 2.2 原理

OpenGL ES 规范要求，当驱动遇到**不认识的枚举值**时，应当产生 `GL_INVALID_ENUM` 错误。反之，如果驱动不报错，则认为扩展存在。

```
流程:
  glGetIntegerv(UNKNOWN_ENUM, ...)  → 驱动不识别
  → glGetError() 返回 GL_INVALID_ENUM  → 判为不支持 ✅

  glGetIntegerv(KNOWN_ENUM, ...)    → 驱动识别
  → glGetError() 返回 GL_NO_ERROR    → 判为支持 ✅
```

### 2.3 局限性分析

#### 问题 1：`glGetIntegerv` 不一定能查询 hint target

`GL_BINNING_CONTROL_HINT_QCOM` 被扩展定义为 **hint target**（传给 `glHint` 的参数），不是 **state pname**（传给 `glGetIntegerv` 的参数）。这两者在 OpenGL ES 规范中属于不同的 enum 命名空间：

| 参数类型 | 用途 | 合法指令 |
|:---|:---|:---|
| Hint target | `glHint(target, mode)` 的第一个参数 | 只用于 `glHint` |
| State pname | `glGetIntegerv(pname, ...)` 的第一个参数 | 只用于 `glGet*` |

扩展规范**没有**将 `GL_BINNING_CONTROL_HINT_QCOM` 列为可查询的 state。因此：

- ✅ 严格驱动：返回 `GL_INVALID_ENUM`（误判为不支持）
- ⚠️ 宽松驱动：可能返回 0 或 `GL_NO_ERROR`（难以判断）

#### 问题 2：glHint 静默忽略

某些驱动实现中，`glHint` 对不认识的 target **静默忽略**而不是报错：

```cpp
glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
// 如果扩展不存在，某些驱动不报错，直接 return
// glGetError() 返回 GL_NO_ERROR → 误判为支持
```

OpenGL ES 3.0 规范原文：

> "If an error is generated, **no changes are made** to the GL state."

但关键问题是——**是否生成错误**取决于驱动实现，规范没有强制要求对未知 hint target 必须报错。

#### 问题 3：不确定的中间状态

`glGetIntegerv` 对非标准 pname 的行为在不同驱动上完全不同：

| 驱动类型 | 返回的错误 | binningHint 值 |
|:---|:---:|:---:|
| 严格（标准） | `GL_INVALID_ENUM` | 不变（-1） |
| 宽松返回 0 | `GL_NO_ERROR` | 0（初始未设置） |
| 宽松返回 hint | `GL_NO_ERROR` | 当前 hint 模式值 |

#### 问题 4：不可靠性全景

```
实际硬件情况 → 探测结果矩阵

                    glGetIntegerv 行为
                ┌─────────┬─────────┬─────────┐
                │ 报错     │ 返回 0  │ 返回值   │
         ┌──────┼─────────┼─────────┼─────────┤
         │ 支持 │ ❌判断  │ ⚠️判断  │ ✅判断  │
glHint   │      │ 为不支持│ 为支持  │ 为支持  │
  行为   ├──────┼─────────┼─────────┼─────────┤
         │ 不报 │ ❌判断  │ ❌判断  │ ❌判断  │
         │ 错   │ 为不支持│ 为支持  │ 为支持  │
         └──────┴─────────┴─────────┴─────────┘
```

**只有绿色区域能得到正确结论，其他区域都会误判。**

### 2.4 结论

> **glError 探测法不可靠。** 它依赖于驱动实现的具体行为（是否对非法枚举报错、是否可查询 hint），而这些行为不在 OpenGL ES 规范的保证范围内。

---

## 3. 探测方法二：扩展字符串检查法

### 3.1 原理

OpenGL ES 提供了标准 API 查询所有支持的扩展。**这是 Khronos 官方推荐的扩展探测方式。**

### 3.2 OpenGL ES 3.0+ 标准方法

```cpp
bool hasGlExtension(const char* targetExtension) {
    GLint numExtensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
    
    for (GLint i = 0; i < numExtensions; i++) {
        const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
        if (ext && strcmp(ext, targetExtension) == 0) {
            return true;
        }
    }
    return false;
}

// 使用
bool hasDirectToFramebuffer = hasGlExtension("GL_QCOM_binning_control");
if (hasDirectToFramebuffer) {
    glEnable(GL_BINNING_CONTROL_HINT_QCOM);
    glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
}
```

### 3.3 OpenGL ES 2.0 兼容方法

```cpp
bool hasGlExtensionCompat(const char* targetExtension) {
    const char* allExtensions = (const char*)glGetString(GL_EXTENSIONS);
    if (!allExtensions) return false;
    
    // 用空格包装，防止部分匹配
    size_t len = strlen(targetExtension);
    const char* ptr = allExtensions;
    while ((ptr = strstr(ptr, targetExtension)) != NULL) {
        // 检查前后的字符都是空格或结尾
        char before = (ptr > allExtensions) ? *(ptr - 1) : ' ';
        char after = *(ptr + len);
        if ((before == ' ' || before == '\0') &&
            (after == ' ' || after == '\0')) {
            return true;
        }
        ptr += len;
    }
    return false;
}
```

### 3.4 为什么这个方法可靠

```
扩展字符串的生命周期:

  GPU 驱动初始化 → 驱动向 GL 注册支持的扩展列表
                    ↓
              glGetStringi(GL_EXTENSIONS, i)
                    ↓
              返回完整的扩展名称列表
                    ↓
              应用检查目标字符串是否存在
                    ↓
              判断结果 = 驱动是否支持此功能
```

**整个流程不依赖错误码，不依赖存储状态，只依赖驱动在初始化时声明的能力列表。** 这是 OpenGL ES 规范唯一明确定义的扩展探测方式。

### 3.5 注意事项

| 注意点 | 说明 |
|:---|:---|
| 上下文绑定 | 必须先有有效的 GL context 才能查询 |
| 区分扩展级别 | 设备级扩展用 `GL_EXTENSIONS`，渲染上下文级用 `eglQueryString` |
| 字符串匹配 | 必须精确匹配（全名），注意空格分隔 |
| 运行时开销 | 建议只查一次并缓存结果 |

---

## 4. 探测方法三：函数指针检查法

### 4.1 原理

某些扩展引入了新的 GL 函数。如果函数指针可以被正确加载，则扩展存在。

### 4.2 示例（不适用于此扩展）

```cpp
// 假设某个扩展引入了 glSpecialFunctionEXT()
// 则可以通过函数指针是否存在来判断

typedef void (GL_APIENTRY* PFNGLSPECIALFUNCTIONEXTPROC)(...);
PFNGLSPECIALFUNCTIONEXTPROC glSpecialFunctionEXT = 
    (PFNGLSPECIALFUNCTIONEXTPROC)eglGetProcAddress("glSpecialFunctionEXT");

if (glSpecialFunctionEXT) {
    // 扩展存在
} else {
    // 扩展不存在
}
```

### 4.3 局限性

`GL_QCOM_binning_control` 扩展**没有引入任何新函数**——它只添加了 `glHint` 的 target/mode 枚举值和 `glEnable` 的 capability。因此函数指针检查法**不适用**于此扩展。

---

## 5. 方法对比与推荐

### 5.1 三种方法全景对比

| 维度 | glError 探测法 | 扩展字符串检查法 | 函数指针检查法 |
|:---|:---:|:---:|:---:|
| **可靠性** | ⚠️ 低 | ✅ **高（规范保证）** | ✅ 高 |
| **兼容范围** | ✅ 所有 GL ES 版本 | ✅ 所有 GL ES 版本 | ✅ 所有版本 |
| **是否可查询 hint 值** | ❌ 不可靠 | ❌ 不涉及 | ❌ 不涉及 |
| **区分扩展具体功能** | ❌ 不能 | ⚠️ 需额外判断 | ✅ 可以直接 |
| **性能开销** | 低（但需多次调用） | 低（建议缓存） | 极低 |
| **规范依据** | ❌ 无（依赖驱动行为） | ✅ Khronos 官方推荐 | ✅ Khronos 标准 |

### 5.2 推荐方案

#### 最佳实践：两步探测法

```cpp
enum class ExtensionState { UNKNOWN, SUPPORTED, UNSUPPORTED };

class GLExtensionProbe {
public:
    static bool hasExtension(const char* extName) {
        // 从缓存查询
        auto it = sCache.find(extName);
        if (it != sCache.end()) return it->second;
        
        // 第一次查询，结果缓存
        bool result = probeExtension(extName);
        sCache[extName] = result;
        return result;
    }

private:
    static std::unordered_map<std::string, bool> sCache;
    
    static bool probeExtension(const char* targetExt) {
        // GL 3.0+ 方式
        GLint numExt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
        if (numExt > 0) {
            for (GLint i = 0; i < numExt; i++) {
                const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
                if (ext && strcmp(ext, targetExt) == 0) return true;
            }
            return false;
        }
        
        // 降级到 GL 2.0 方式
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
};

// 使用
if (GLExtensionProbe::hasExtension("GL_QCOM_binning_control")) {
    LOGI("GL_QCOM_binning_control supported, enabling Direct Mode");
    glEnable(GL_BINNING_CONTROL_HINT_QCOM);
    glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM);
} else {
    LOGW("GL_QCOM_binning_control not supported, using fallback path");
}
```

---

## 6. Snapdragon SDK 的做法

### 6.1 代码

```cpp
// svrApiTimeWarp.cpp:7730
if (gDirectModeWarp)
{
    LOGI("Rendering in direct mode");
    GL( glEnable(GL_BINNING_CONTROL_HINT_QCOM) );
    GL( glHint(GL_BINNING_CONTROL_HINT_QCOM, GL_RENDER_DIRECT_TO_FRAMEBUFFER_QCOM) );
}
```

### 6.2 分析

Snapdragon XR SDK **不做任何探测**，直接启用。原因：

| 原因 | 说明 |
|:---|:---|
| 目标平台固定 | 只运行在高通 Adreno GPU 上，扩展必然存在 |
| 运行风险低 | 如果扩展不存在，`glHint` 设置无效，渲染结果退化为标准 tiling 模式，**不会崩溃** |
| 历史原因 | 此 SDK 最初为特定设备定制，不需要跨厂商兼容 |
| SDK 定位 | XR SDK 是设备厂商集成层，不是通用应用框架 |

### 6.3 启示

> **如果你的软件只运行在已知硬件平台上，直接启用扩展而不探测是合理的。**
>
> **如果你的软件需要跨平台/跨厂商兼容，必须使用扩展字符串检查法进行探测。**

---

## 总结

| 方法 | 可靠性 | 适用场景 |
|:---|:---:|:---|
| **glError 探测法** | ❌ 不可靠 | 不推荐在任何场景使用 |
| **扩展字符串检查法** | ✅ 可靠 | 所有跨平台/跨厂商场景，**推荐使用** |
| **函数指针检查法** | ✅ 可靠 | 仅用于引入了新 GL 函数的扩展 |
| **直接启用（不做探测）** | ⚠️ 适用于已知平台 | 硬件平台固定的场景，如设备厂商 SDK |
