/* Copyright (c) 2015-2020 The Khronos Group Inc.
 * Copyright (c) 2015-2020 Valve Corporation
 * Copyright (c) 2015-2020 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Courtney Goeltzenleuchter <courtney@LunarG.com>
 * Author: Tobin Ehlis <tobin@lunarg.com>
 * Author: Mark Young <marky@lunarg.com>
 * Author: Dave Houlton <daveh@lunarg.com>
 *
 */

#ifndef LAYER_LOGGING_H
#define LAYER_LOGGING_H

#include <cinttypes>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

#include "vk_typemap_helper.h"
#include "vk_layer_config.h"
#include "vk_layer_data.h"
#include "vk_loader_platform.h"
#include "vulkan/vk_layer.h"
#include "vk_object_types.h"
#include "cast_utils.h"
#include "vk_validation_error_messages.h"
#include "vk_layer_dispatch_table.h"
#include "vk_safe_struct.h"

// Suppress unused warning on Linux
#if defined(__GNUC__)
#define DECORATE_UNUSED __attribute__((unused))
#else
#define DECORATE_UNUSED
#endif

#if defined __ANDROID__
#include <android/log.h>
#define LOGCONSOLE(...) ((void)__android_log_print(ANDROID_LOG_INFO, "VALIDATION", __VA_ARGS__))
#else
#define LOGCONSOLE(...)      \
    {                        \
        printf(__VA_ARGS__); \
        printf("\n");        \
    }
#endif

static const char DECORATE_UNUSED *kVUIDUndefined = "VUID_Undefined";

#undef DECORATE_UNUSED

typedef enum DebugCallbackStatusBits {
    DEBUG_CALLBACK_UTILS = 0x00000001,     // This struct describes a VK_EXT_debug_utils callback
    DEBUG_CALLBACK_DEFAULT = 0x00000002,   // An internally created callback, used if no user-defined callbacks are registered
    DEBUG_CALLBACK_INSTANCE = 0x00000004,  // An internally created temporary instance callback
} DebugCallbackStatusBits;
typedef VkFlags DebugCallbackStatusFlags;

typedef struct {
    DebugCallbackStatusFlags callback_status;

    // Debug report related information
    VkDebugReportCallbackEXT debug_report_callback_object;
    PFN_vkDebugReportCallbackEXT debug_report_callback_function_ptr;
    VkFlags debug_report_msg_flags;

    // Debug utils related information
    VkDebugUtilsMessengerEXT debug_utils_callback_object;
    VkDebugUtilsMessageSeverityFlagsEXT debug_utils_msg_flags;
    VkDebugUtilsMessageTypeFlagsEXT debug_utils_msg_type;
    PFN_vkDebugUtilsMessengerCallbackEXT debug_utils_callback_function_ptr;

    void *pUserData;

    bool IsUtils() { return ((callback_status & DEBUG_CALLBACK_UTILS) != 0); }
    bool IsDefault() { return ((callback_status & DEBUG_CALLBACK_DEFAULT) != 0); }
    bool IsInstance() { return ((callback_status & DEBUG_CALLBACK_INSTANCE) != 0); }
} VkLayerDbgFunctionState;

// TODO: Could be autogenerated for the specific handles for extra type safety...
template <typename HANDLE_T>
static inline uint64_t HandleToUint64(HANDLE_T h) {
    return CastToUint64<HANDLE_T>(h);
}

static inline uint64_t HandleToUint64(uint64_t h) { return h; }

// Data we store per label for logging
struct LoggingLabel {
    std::string name;
    std::array<float, 4> color;

    void Reset() { *this = LoggingLabel(); }
    bool Empty() const { return name.empty(); }

    VkDebugUtilsLabelEXT Export() const {
        auto out = lvl_init_struct<VkDebugUtilsLabelEXT>();
        out.pLabelName = name.c_str();
        std::copy(color.cbegin(), color.cend(), out.color);
        return out;
    };

    LoggingLabel() : name(), color({{0.f, 0.f, 0.f, 0.f}}) {}
    LoggingLabel(const VkDebugUtilsLabelEXT *label_info) {
        if (label_info && label_info->pLabelName) {
            name = label_info->pLabelName;
            std::copy_n(std::begin(label_info->color), 4, color.begin());
        } else {
            Reset();
        }
    }

    LoggingLabel(const LoggingLabel &) = default;
    LoggingLabel &operator=(const LoggingLabel &) = default;
    LoggingLabel &operator=(LoggingLabel &&) = default;
    LoggingLabel(LoggingLabel &&) = default;

    template <typename Name, typename Vec>
    LoggingLabel(Name &&name_, Vec &&vec_) : name(std::forward<Name>(name_)), color(std::forward<Vec>(vec_)) {}
};

struct LoggingLabelState {
    std::vector<LoggingLabel> labels;
    LoggingLabel insert_label;

    // Export the labels, but in reverse order since we want the most recent at the top.
    std::vector<VkDebugUtilsLabelEXT> Export() const {
        size_t count = labels.size() + (insert_label.Empty() ? 0 : 1);
        std::vector<VkDebugUtilsLabelEXT> out(count);

        if (!count) return out;

        size_t index = count - 1;
        if (!insert_label.Empty()) {
            out[index--] = insert_label.Export();
        }
        for (const auto &label : labels) {
            out[index--] = label.Export();
        }
        return out;
    }
};

static inline int string_sprintf(std::string *output, const char *fmt, ...);

typedef struct _debug_report_data {
    std::vector<VkLayerDbgFunctionState> debug_callback_list;
    VkDebugUtilsMessageSeverityFlagsEXT active_severities{0};
    VkDebugUtilsMessageTypeFlagsEXT active_types{0};
    bool queueLabelHasInsert{false};
    bool cmdBufLabelHasInsert{false};
    std::unordered_map<uint64_t, std::string> debugObjectNameMap;
    std::unordered_map<uint64_t, std::string> debugUtilsObjectNameMap;
    std::unordered_map<VkQueue, std::unique_ptr<LoggingLabelState>> debugUtilsQueueLabels;
    std::unordered_map<VkCommandBuffer, std::unique_ptr<LoggingLabelState>> debugUtilsCmdBufLabels;
    // This mutex is defined as mutable since the normal usage for a debug report object is as 'const'. The mutable keyword allows
    // the layers to continue this pattern, but also allows them to use/change this specific member for synchronization purposes.
    mutable std::mutex debug_output_mutex;
    const void *instance_pnext_chain{};

    void DebugReportSetUtilsObjectName(const VkDebugUtilsObjectNameInfoEXT *pNameInfo) {
        std::unique_lock<std::mutex> lock(debug_output_mutex);
        if (pNameInfo->pObjectName) {
            debugUtilsObjectNameMap[pNameInfo->objectHandle] = pNameInfo->pObjectName;
        } else {
            debugUtilsObjectNameMap.erase(pNameInfo->objectHandle);
        }
    }

    void DebugReportSetMarkerObjectName(const VkDebugMarkerObjectNameInfoEXT *pNameInfo) {
        std::unique_lock<std::mutex> lock(debug_output_mutex);
        if (pNameInfo->pObjectName) {
            debugObjectNameMap[pNameInfo->object] = pNameInfo->pObjectName;
        } else {
            debugObjectNameMap.erase(pNameInfo->object);
        }
    }

    std::string DebugReportGetUtilsObjectName(const uint64_t object) const {
        std::string label = "";
        const auto utils_name_iter = debugUtilsObjectNameMap.find(object);
        if (utils_name_iter != debugUtilsObjectNameMap.end()) {
            label = utils_name_iter->second;
        }
        return label;
    }

    std::string DebugReportGetMarkerObjectName(const uint64_t object) const {
        std::string label = "";
        const auto marker_name_iter = debugObjectNameMap.find(object);
        if (marker_name_iter != debugObjectNameMap.end()) {
            label = marker_name_iter->second;
        }
        return label;
    }

    std::string FormatHandle(const char *handle_type_name, uint64_t handle) const {
        std::string handle_name = DebugReportGetUtilsObjectName(handle);
        if (handle_name.empty()) {
            handle_name = DebugReportGetMarkerObjectName(handle);
        }

        std::string ret;
        string_sprintf(&ret, "%s 0x%" PRIxLEAST64 "[%s]", handle_type_name, handle, handle_name.c_str());
        return ret;
    }

    std::string FormatHandle(const VulkanTypedHandle &handle) const {
        return FormatHandle(object_string[handle.type], handle.handle);
    }

    template <typename HANDLE_T>
    std::string FormatHandle(HANDLE_T handle) const {
        return FormatHandle(VkHandleInfo<HANDLE_T>::Typename(), HandleToUint64(handle));
    }

} debug_report_data;

template debug_report_data *GetLayerDataPtr<debug_report_data>(void *data_key,
                                                               std::unordered_map<void *, debug_report_data *> &data_map);

static inline void DebugReportFlagsToAnnotFlags(VkDebugReportFlagsEXT dr_flags, bool default_flag_is_spec,
                                                VkDebugUtilsMessageSeverityFlagsEXT *da_severity,
                                                VkDebugUtilsMessageTypeFlagsEXT *da_type) {
    *da_severity = 0;
    *da_type = 0;
    // If it's explicitly listed as a performance warning, treat it as a performance message. Otherwise, treat it as a validation
    // issue.
    if ((dr_flags & VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    if ((dr_flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    }
    if ((dr_flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    }
    if ((dr_flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    if ((dr_flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0) {
        *da_type |= VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        *da_severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    }
}

// Forward Declarations
static inline bool debug_log_msg(const debug_report_data *debug_data, VkFlags msg_flags, VkDebugReportObjectTypeEXT object_type,
                                 uint64_t src_object, size_t location, const char *layer_prefix, const char *message,
                                 const char *text_vuid);

static void SetDebugUtilsSeverityFlags(std::vector<VkLayerDbgFunctionState> &callbacks, debug_report_data *debug_data) {
    // For all callback in list, return their complete set of severities and modes
    for (auto item : callbacks) {
        if (item.IsUtils()) {
            debug_data->active_severities |= item.debug_utils_msg_flags;
            debug_data->active_types |= item.debug_utils_msg_type;
        } else {
            VkFlags severities = 0;
            VkFlags types = 0;
            DebugReportFlagsToAnnotFlags(item.debug_report_msg_flags, true, &severities, &types);
            debug_data->active_severities |= severities;
            debug_data->active_types |= types;
        }
    }
}

static inline void RemoveDebugUtilsCallback(debug_report_data *debug_data, std::vector<VkLayerDbgFunctionState> &callbacks,
                                            uint64_t callback) {
    auto item = callbacks.begin();
    for (item = callbacks.begin(); item != callbacks.end(); item++) {
        if (item->IsUtils()) {
            if (item->debug_utils_callback_object == CastToHandle<VkDebugUtilsMessengerEXT>(callback)) break;
        } else {
            if (item->debug_report_callback_object == CastToHandle<VkDebugReportCallbackEXT>(callback)) break;
        }
    }
    if (item != callbacks.end()) {
        callbacks.erase(item);
    }
    SetDebugUtilsSeverityFlags(callbacks, debug_data);
}

// Deletes all debug callback function structs
static inline void RemoveAllMessageCallbacks(debug_report_data *debug_data, std::vector<VkLayerDbgFunctionState> &callbacks) {
    callbacks.clear();
}

static inline bool debug_log_msg(const debug_report_data *debug_data, VkFlags msg_flags, VkDebugReportObjectTypeEXT object_type,
                                 uint64_t src_object, size_t location, const char *layer_prefix, const char *message,
                                 const char *text_vuid) {
    bool bail = false;

    VkDebugUtilsMessageSeverityFlagsEXT severity;
    VkDebugUtilsMessageTypeFlagsEXT types;
    VkDebugUtilsMessengerCallbackDataEXT callback_data;
    VkDebugUtilsObjectNameInfoEXT object_name_info;

    // Convert the info to the VK_EXT_debug_utils form in case we need it.
    DebugReportFlagsToAnnotFlags(msg_flags, true, &severity, &types);
    object_name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    object_name_info.pNext = NULL;
    object_name_info.objectType = convertDebugReportObjectToCoreObject(object_type);
    object_name_info.objectHandle = (uint64_t)(uintptr_t)src_object;
    object_name_info.pObjectName = NULL;
    std::string object_label = {};

    callback_data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
    callback_data.pNext = NULL;
    callback_data.flags = 0;
    callback_data.pMessageIdName = text_vuid;
    callback_data.messageIdNumber = 0;  // deprecated, validation layers use only the pMessageIdName
    callback_data.pMessage = message;
    callback_data.queueLabelCount = 0;
    callback_data.pQueueLabels = NULL;
    callback_data.cmdBufLabelCount = 0;
    callback_data.pCmdBufLabels = NULL;
    callback_data.objectCount = 1;
    callback_data.pObjects = &object_name_info;

    std::vector<VkDebugUtilsLabelEXT> queue_labels;
    std::vector<VkDebugUtilsLabelEXT> cmd_buf_labels;
    std::string new_debug_report_message = "";
    std::ostringstream oss;

    if (0 != src_object) {
        oss << "Object: 0x" << std::hex << src_object;
        // If this is a queue, add any queue labels to the callback data.
        if (VK_OBJECT_TYPE_QUEUE == object_name_info.objectType) {
            auto label_iter = debug_data->debugUtilsQueueLabels.find(reinterpret_cast<VkQueue>(src_object));
            if (label_iter != debug_data->debugUtilsQueueLabels.end()) {
                queue_labels = label_iter->second->Export();
                callback_data.queueLabelCount = static_cast<uint32_t>(queue_labels.size());
                callback_data.pQueueLabels = queue_labels.empty() ? nullptr : queue_labels.data();
            }
            // If this is a command buffer, add any command buffer labels to the callback data.
        } else if (VK_OBJECT_TYPE_COMMAND_BUFFER == object_name_info.objectType) {
            auto label_iter = debug_data->debugUtilsCmdBufLabels.find(reinterpret_cast<VkCommandBuffer>(src_object));
            if (label_iter != debug_data->debugUtilsCmdBufLabels.end()) {
                cmd_buf_labels = label_iter->second->Export();
                callback_data.cmdBufLabelCount = static_cast<uint32_t>(cmd_buf_labels.size());
                callback_data.pCmdBufLabels = cmd_buf_labels.empty() ? nullptr : cmd_buf_labels.data();
            }
        }

        // Look for any debug utils or marker names to use for this object
        object_label = debug_data->DebugReportGetUtilsObjectName(src_object);
        if (object_label.empty()) {
            object_label = debug_data->DebugReportGetMarkerObjectName(src_object);
        }
        if (!object_label.empty()) {
            object_name_info.pObjectName = object_label.c_str();
            oss << " (Name = " << object_label << " : Type = ";
        } else {
            oss << " (Type = ";
        }
        oss << std::to_string(object_type) << ")";
    } else {
        oss << "Object: VK_NULL_HANDLE (Type = " << std::to_string(object_type) << ")";
    }
    new_debug_report_message += oss.str();
    new_debug_report_message += " | ";
    new_debug_report_message += message;

    const auto callback_list = &debug_data->debug_callback_list;

    // We only output to default callbacks if there are no non-default callbacks
    bool use_default_callbacks = true;
    for (auto current_callback : *callback_list) {
        use_default_callbacks &= current_callback.IsDefault();
    }

    for (auto current_callback : *callback_list) {
        // Skip callback if it's a default callback and there are non-default callbacks present
        if (current_callback.IsDefault() && !use_default_callbacks) continue;

        // VK_EXT_debug_report callback (deprecated)
        if (!current_callback.IsUtils() && (current_callback.debug_report_msg_flags & msg_flags)) {
            if (text_vuid != nullptr) {
                // If a text vuid is supplied for the old debug report extension, prepend it to the message string
                new_debug_report_message.insert(0, " ] ");
                new_debug_report_message.insert(0, text_vuid);
                new_debug_report_message.insert(0, " [ ");
            }
            if (current_callback.debug_report_callback_function_ptr(msg_flags, object_type, src_object, location, 0, layer_prefix,
                                                                    new_debug_report_message.c_str(), current_callback.pUserData)) {
                bail = true;
            }
            // VK_EXT_debug_utils callback
        } else if (current_callback.IsUtils() && (current_callback.debug_utils_msg_flags & severity) &&
                   (current_callback.debug_utils_msg_type & types)) {
            if (current_callback.debug_utils_callback_function_ptr(static_cast<VkDebugUtilsMessageSeverityFlagBitsEXT>(severity),
                                                                   types, &callback_data, current_callback.pUserData)) {
                bail = true;
            }
        }
    }
    return bail;
}

static inline void DebugAnnotFlagsToReportFlags(VkDebugUtilsMessageSeverityFlagBitsEXT da_severity,
                                                VkDebugUtilsMessageTypeFlagsEXT da_type, VkDebugReportFlagsEXT *dr_flags) {
    *dr_flags = 0;

    if ((da_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        *dr_flags |= VK_DEBUG_REPORT_ERROR_BIT_EXT;
    } else if ((da_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        if ((da_type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) {
            *dr_flags |= VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        } else {
            *dr_flags |= VK_DEBUG_REPORT_WARNING_BIT_EXT;
        }
    } else if ((da_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        *dr_flags |= VK_DEBUG_REPORT_INFORMATION_BIT_EXT;
    } else if ((da_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        *dr_flags |= VK_DEBUG_REPORT_DEBUG_BIT_EXT;
    }
}

static inline void layer_debug_utils_destroy_instance(debug_report_data *debug_data) {
    if (debug_data) {
        std::unique_lock<std::mutex> lock(debug_data->debug_output_mutex);
        RemoveAllMessageCallbacks(debug_data, debug_data->debug_callback_list);
        lock.unlock();
        delete (debug_data);
    }
}

template <typename T>
static inline void layer_destroy_callback(debug_report_data *debug_data, T callback, const VkAllocationCallbacks *allocator) {
    std::unique_lock<std::mutex> lock(debug_data->debug_output_mutex);
    RemoveDebugUtilsCallback(debug_data, debug_data->debug_callback_list, CastToUint64(callback));
}

template <typename TCreateInfo, typename TCallback>
static inline void layer_create_callback(DebugCallbackStatusFlags callback_status, debug_report_data *debug_data,
                                         const TCreateInfo *create_info, const VkAllocationCallbacks *allocator,
                                         TCallback *callback) {
    std::unique_lock<std::mutex> lock(debug_data->debug_output_mutex);

    debug_data->debug_callback_list.emplace_back(VkLayerDbgFunctionState());
    auto &callback_state = debug_data->debug_callback_list.back();
    callback_state.callback_status = callback_status;
    callback_state.pUserData = create_info->pUserData;

    if (callback_state.IsUtils()) {
        auto utils_create_info = reinterpret_cast<const VkDebugUtilsMessengerCreateInfoEXT *>(create_info);
        auto utils_callback = reinterpret_cast<VkDebugUtilsMessengerEXT *>(callback);
        if (!(*utils_callback)) {
            // callback constructed default callbacks have no handle -- so use struct address as unique handle
            *utils_callback = reinterpret_cast<VkDebugUtilsMessengerEXT>(&callback_state);
        }
        callback_state.debug_utils_callback_object = *utils_callback;
        callback_state.debug_utils_callback_function_ptr = utils_create_info->pfnUserCallback;
        callback_state.debug_utils_msg_flags = utils_create_info->messageSeverity;
        callback_state.debug_utils_msg_type = utils_create_info->messageType;
    } else {  // Debug report callback
        auto report_create_info = reinterpret_cast<const VkDebugReportCallbackCreateInfoEXT *>(create_info);
        auto report_callback = reinterpret_cast<VkDebugReportCallbackEXT *>(callback);
        if (!(*report_callback)) {
            // Internally constructed default callbacks have no handle -- so use struct address as unique handle
            *report_callback = reinterpret_cast<VkDebugReportCallbackEXT>(&callback_state);
        }
        callback_state.debug_report_callback_object = *report_callback;
        callback_state.debug_report_callback_function_ptr = report_create_info->pfnCallback;
        callback_state.debug_report_msg_flags = report_create_info->flags;
    }

    SetDebugUtilsSeverityFlags(debug_data->debug_callback_list, debug_data);
}

static inline VkResult layer_create_messenger_callback(debug_report_data *debug_data, bool default_callback,
                                                       const VkDebugUtilsMessengerCreateInfoEXT *create_info,
                                                       const VkAllocationCallbacks *allocator,
                                                       VkDebugUtilsMessengerEXT *messenger) {
    layer_create_callback((DEBUG_CALLBACK_UTILS | (default_callback ? DEBUG_CALLBACK_DEFAULT : 0)), debug_data, create_info,
                          allocator, messenger);
    return VK_SUCCESS;
}

static inline VkResult layer_create_report_callback(debug_report_data *debug_data, bool default_callback,
                                                    const VkDebugReportCallbackCreateInfoEXT *create_info,
                                                    const VkAllocationCallbacks *allocator, VkDebugReportCallbackEXT *callback) {
    layer_create_callback((default_callback ? DEBUG_CALLBACK_DEFAULT : 0), debug_data, create_info, allocator, callback);
    return VK_SUCCESS;
}

static inline void ActivateInstanceDebugCallbacks(debug_report_data *debug_data) {
    auto current = debug_data->instance_pnext_chain;
    for (;;) {
        auto create_info = lvl_find_in_chain<VkDebugUtilsMessengerCreateInfoEXT>(current);
        if (!create_info) break;
        current = create_info->pNext;
        VkDebugUtilsMessengerEXT utils_callback{};
        layer_create_callback((DEBUG_CALLBACK_UTILS | DEBUG_CALLBACK_INSTANCE), debug_data, create_info, nullptr, &utils_callback);
    }
    for (;;) {
        auto create_info = lvl_find_in_chain<VkDebugReportCallbackCreateInfoEXT>(current);
        if (!create_info) break;
        current = create_info->pNext;
        VkDebugReportCallbackEXT report_callback{};
        layer_create_callback(DEBUG_CALLBACK_INSTANCE, debug_data, create_info, nullptr, &report_callback);
    }
}

static inline void DeactivateInstanceDebugCallbacks(debug_report_data *debug_data) {
    if (!lvl_find_in_chain<VkDebugUtilsMessengerCreateInfoEXT>(debug_data->instance_pnext_chain) &&
        !lvl_find_in_chain<VkDebugReportCallbackCreateInfoEXT>(debug_data->instance_pnext_chain))
        return;
    std::vector<VkDebugUtilsMessengerEXT> instance_utils_callback_handles{};
    std::vector<VkDebugReportCallbackEXT> instance_report_callback_handles{};
    for (auto item : debug_data->debug_callback_list) {
        if (item.IsInstance()) {
            if (item.IsUtils()) {
                instance_utils_callback_handles.push_back(item.debug_utils_callback_object);
            } else {
                instance_report_callback_handles.push_back(item.debug_report_callback_object);
            }
        }
    }
    for (auto item : instance_utils_callback_handles) {
        layer_destroy_callback(debug_data, item, nullptr);
    }
    for (auto item : instance_report_callback_handles) {
        layer_destroy_callback(debug_data, item, nullptr);
    }
}

#ifndef WIN32
static inline int string_sprintf(std::string *output, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static inline int string_sprintf(std::string *output, const char *fmt, ...) {
    std::string &formatted = *output;
    va_list argptr;
    va_start(argptr, fmt);
    int reserve = vsnprintf(nullptr, 0, fmt, argptr);
    va_end(argptr);
    formatted.reserve(reserve + 1);  // Set the storage length long enough to hold the output + null
    formatted.resize(reserve);       // Set the *logical* length to be what vsprintf will write
    va_start(argptr, fmt);
    int result = vsnprintf((char *)formatted.data(), formatted.capacity(), fmt, argptr);
    va_end(argptr);
    assert(result == reserve);
    assert((formatted.size() == strlen(formatted.c_str())));
    return result;
}

#ifdef WIN32
static inline int vasprintf(char **strp, char const *fmt, va_list ap) {
    *strp = nullptr;
    int size = _vscprintf(fmt, ap);
    if (size >= 0) {
        *strp = (char *)malloc(size + 1);
        if (!*strp) {
            return -1;
        }
        _vsnprintf(*strp, size + 1, fmt, ap);
    }
    return size;
}
#endif

// This must be called with the debug_output_mutex already held
static inline bool LogMsgLocked(const debug_report_data *debug_data, VkFlags msg_flags, VkDebugReportObjectTypeEXT object_type,
                                uint64_t src_object, const std::string &vuid_text, char *err_msg) {
    std::string str_plus_spec_text(err_msg ? err_msg : "Allocation failure");

    // Append the spec error text to the error message, unless it's an UNASSIGNED or UNDEFINED vuid
    if ((vuid_text.find("UNASSIGNED-") == std::string::npos) && (vuid_text.find(kVUIDUndefined) == std::string::npos)) {
        // Linear search makes no assumptions about the layout of the string table. This is not fast, but it does not need to be at
        // this point in the error reporting path
        uint32_t num_vuids = sizeof(vuid_spec_text) / sizeof(vuid_spec_text_pair);
        const char *spec_text = nullptr;
        for (uint32_t i = 0; i < num_vuids; i++) {
            if (0 == strcmp(vuid_text.c_str(), vuid_spec_text[i].vuid)) {
                spec_text = vuid_spec_text[i].spec_text;
                break;
            }
        }

        if (nullptr == spec_text) {
            // If this happens, you've hit a VUID string that isn't defined in the spec's json file
            // Try running 'vk_validation_stats -c' to look for invalid VUID strings in the repo code
            assert(0);
        } else {
            str_plus_spec_text += " The Vulkan spec states: ";
            str_plus_spec_text += spec_text;
        }
    }

    // Append layer prefix with VUID string, pass in recovered legacy numerical VUID

    bool result = debug_log_msg(debug_data, msg_flags, object_type, src_object, 0, "Validation", str_plus_spec_text.c_str(),
                                vuid_text.c_str());

    free(err_msg);
    return result;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL report_log_callback(VkFlags msg_flags, VkDebugReportObjectTypeEXT obj_type,
                                                                 uint64_t src_object, size_t location, int32_t msg_code,
                                                                 const char *layer_prefix, const char *message, void *user_data) {
    std::ostringstream msg_buffer;
    char msg_flag_string[30];

    PrintMessageFlags(msg_flags, msg_flag_string);

    msg_buffer << layer_prefix << "(" << msg_flag_string << "): msg_code: " << msg_code << ": " << message << "\n";
    const std::string tmp = msg_buffer.str();
    const char *cstr = tmp.c_str();

    fprintf((FILE *)user_data, "%s", cstr);
    fflush((FILE *)user_data);

#if defined __ANDROID__
    LOGCONSOLE("%s", cstr);
#endif

    return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL report_win32_debug_output_msg(VkFlags msg_flags, VkDebugReportObjectTypeEXT obj_type,
                                                                           uint64_t src_object, size_t location, int32_t msg_code,
                                                                           const char *layer_prefix, const char *message,
                                                                           void *user_data) {
#ifdef WIN32
    char msg_flag_string[30];
    char buf[2048];

    PrintMessageFlags(msg_flags, msg_flag_string);
    _snprintf(buf, sizeof(buf) - 1, "%s (%s): msg_code: %d: %s\n", layer_prefix, msg_flag_string, msg_code, message);

    OutputDebugString(buf);
#endif

    return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL DebugBreakCallback(VkFlags msgFlags, VkDebugReportObjectTypeEXT obj_type,
                                                                uint64_t src_object, size_t location, int32_t msg_code,
                                                                const char *layer_prefix, const char *message, void *user_data) {
#ifdef WIN32
    DebugBreak();
#else
    raise(SIGTRAP);
#endif

    return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL MessengerBreakCallback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                                    VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                                    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                                                                    void *user_data) {
#ifdef WIN32
    DebugBreak();
#else
    raise(SIGTRAP);
#endif

    return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL messenger_log_callback(VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
                                                                    VkDebugUtilsMessageTypeFlagsEXT message_type,
                                                                    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
                                                                    void *user_data) {
    std::ostringstream msg_buffer;
    char msg_severity[30];
    char msg_type[30];

    PrintMessageSeverity(message_severity, msg_severity);
    PrintMessageType(message_type, msg_type);

    msg_buffer << callback_data->pMessageIdName << "(" << msg_severity << " / " << msg_type
               << "): msgNum: " << callback_data->messageIdNumber << " - " << callback_data->pMessage << "\n";
    msg_buffer << "    Objects: " << callback_data->objectCount << "\n";
    for (uint32_t obj = 0; obj < callback_data->objectCount; ++obj) {
        msg_buffer << "        [" << obj << "] " << std::hex << std::showbase
                   << HandleToUint64(callback_data->pObjects[obj].objectHandle) << ", type: " << std::dec << std::noshowbase
                   << callback_data->pObjects[obj].objectType
                   << ", name: " << (callback_data->pObjects[obj].pObjectName ? callback_data->pObjects[obj].pObjectName : "NULL")
                   << "\n";
    }
    const std::string tmp = msg_buffer.str();
    const char *cstr = tmp.c_str();
    fprintf((FILE *)user_data, "%s", cstr);
    fflush((FILE *)user_data);

#if defined __ANDROID__
    LOGCONSOLE("%s", cstr);
#endif

    return false;
}

static inline VKAPI_ATTR VkBool32 VKAPI_CALL messenger_win32_debug_output_msg(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity, VkDebugUtilsMessageTypeFlagsEXT message_type,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data, void *user_data) {
#ifdef WIN32
    std::ostringstream msg_buffer;
    char msg_severity[30];
    char msg_type[30];

    PrintMessageSeverity(message_severity, msg_severity);
    PrintMessageType(message_type, msg_type);

    msg_buffer << callback_data->pMessageIdName << "(" << msg_severity << " / " << msg_type
               << "): msgNum: " << callback_data->messageIdNumber << " - " << callback_data->pMessage << "\n";
    msg_buffer << "    Objects: " << callback_data->objectCount << "\n";

    for (uint32_t obj = 0; obj < callback_data->objectCount; ++obj) {
        msg_buffer << "       [" << obj << "]  " << std::hex << std::showbase
                   << HandleToUint64(callback_data->pObjects[obj].objectHandle) << ", type: " << std::dec << std::noshowbase
                   << callback_data->pObjects[obj].objectType
                   << ", name: " << (callback_data->pObjects[obj].pObjectName ? callback_data->pObjects[obj].pObjectName : "NULL")
                   << "\n";
    }
    const std::string tmp = msg_buffer.str();
    const char *cstr = tmp.c_str();
    OutputDebugString(cstr);
#endif

    return false;
}

template <typename Map>
static LoggingLabelState *GetLoggingLabelState(Map *map, typename Map::key_type key, bool insert) {
    auto iter = map->find(key);
    LoggingLabelState *label_state = nullptr;
    if (iter == map->end()) {
        if (insert) {
            // Add a label state if not present
            auto inserted = map->insert(std::make_pair(key, std::unique_ptr<LoggingLabelState>(new LoggingLabelState())));
            assert(inserted.second);
            iter = inserted.first;
            label_state = iter->second.get();
        }
    } else {
        label_state = iter->second.get();
    }
    return label_state;
}

static inline void BeginQueueDebugUtilsLabel(debug_report_data *report_data, VkQueue queue,
                                             const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    if (nullptr != label_info && nullptr != label_info->pLabelName) {
        auto *label_state = GetLoggingLabelState(&report_data->debugUtilsQueueLabels, queue, /* insert */ true);
        assert(label_state);
        label_state->labels.push_back(LoggingLabel(label_info));

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

static inline void EndQueueDebugUtilsLabel(debug_report_data *report_data, VkQueue queue) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&report_data->debugUtilsQueueLabels, queue, /* insert */ false);
    if (label_state) {
        // Pop the normal item
        if (!label_state->labels.empty()) {
            label_state->labels.pop_back();
        }

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

static inline void InsertQueueDebugUtilsLabel(debug_report_data *report_data, VkQueue queue,
                                              const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&report_data->debugUtilsQueueLabels, queue, /* insert */ true);

    // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
    label_state->insert_label = LoggingLabel(label_info);
}

static inline void BeginCmdDebugUtilsLabel(debug_report_data *report_data, VkCommandBuffer command_buffer,
                                           const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    if (nullptr != label_info && nullptr != label_info->pLabelName) {
        auto *label_state = GetLoggingLabelState(&report_data->debugUtilsCmdBufLabels, command_buffer, /* insert */ true);
        assert(label_state);
        label_state->labels.push_back(LoggingLabel(label_info));

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

static inline void EndCmdDebugUtilsLabel(debug_report_data *report_data, VkCommandBuffer command_buffer) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&report_data->debugUtilsCmdBufLabels, command_buffer, /* insert */ false);
    if (label_state) {
        // Pop the normal item
        if (!label_state->labels.empty()) {
            label_state->labels.pop_back();
        }

        // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
        label_state->insert_label.Reset();
    }
}

static inline void InsertCmdDebugUtilsLabel(debug_report_data *report_data, VkCommandBuffer command_buffer,
                                            const VkDebugUtilsLabelEXT *label_info) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&report_data->debugUtilsCmdBufLabels, command_buffer, /* insert */ true);
    assert(label_state);

    // TODO: Determine if this is the correct semantics for insert label vs. begin/end, perserving existing semantics for now
    label_state->insert_label = LoggingLabel(label_info);
}

// Current tracking beyond a single command buffer scope is incorrect, and even when it is we need to be able to clean up
static inline void ResetCmdDebugUtilsLabel(debug_report_data *report_data, VkCommandBuffer command_buffer) {
    std::unique_lock<std::mutex> lock(report_data->debug_output_mutex);
    auto *label_state = GetLoggingLabelState(&report_data->debugUtilsCmdBufLabels, command_buffer, /* insert */ false);
    if (label_state) {
        label_state->labels.clear();
        label_state->insert_label.Reset();
    }
}

static inline void EraseCmdDebugUtilsLabel(debug_report_data *report_data, VkCommandBuffer command_buffer) {
    report_data->debugUtilsCmdBufLabels.erase(command_buffer);
}

#endif  // LAYER_LOGGING_H
