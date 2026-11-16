#ifndef VIDEO_TRANSMISSION_H
#define VIDEO_TRANSMISSION_H

/* Single distributable public header. All ABI declarations and tunable
 * session/reference transport parameters are available from this file. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT_ABI_VERSION 1u
#if defined(_WIN32) && defined(VT_BUILDING_LIBRARY)
#define VT_API __declspec(dllexport)
#elif defined(_WIN32)
#define VT_API __declspec(dllimport)
#else
#define VT_API __attribute__((visibility("default")))
#endif

typedef struct vt_session vt_session;

typedef enum vt_status {
    VT_OK = 0,
    VT_INVALID_ARGUMENT = 1,
    VT_NOT_READY = 2,
    VT_QUEUE_FULL = 3,
    VT_UNSUPPORTED = 4,
    VT_IO_ERROR = 5,
    VT_CANCELLED = 6
} vt_status;

typedef enum vt_role { VT_SKY = 1, VT_GROUND = 2 } vt_role;
typedef enum vt_input_mode { VT_INPUT_ENCODED = 1, VT_INPUT_RAW = 2 } vt_input_mode;
typedef enum vt_event_type {
    VT_EVENT_READY = 1,
    VT_EVENT_LINK_DOWN = 2,
    VT_EVENT_LINK_UP = 3,
    VT_EVENT_FRAME_DROPPED = 4,
    VT_EVENT_KEYFRAME_NEEDED = 5,
    VT_EVENT_ERROR = 6
} vt_event_type;

enum { VT_FRAME_KEYFRAME = 1u, VT_FRAME_PARAMETER_SETS = 2u };

/* One complete H.265 access unit. Input bytes are copied on accepted submit.
 * Callback bytes are borrowed until that callback returns. PTS is a stream
 * timestamp; it is not a clock synchronized between devices. */
typedef struct vt_encoded_frame {
    uint32_t struct_size;
    const uint8_t* data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint64_t frame_number;
    uint64_t pts_us;
    uint32_t flags;
} vt_encoded_frame;

/* Raw storage belongs to the producer. The session calls retain on accepted
 * submit and release when it is done. Media backends retain again if they use
 * the storage after their encode call returns. native_buffer is interpreted
 * only by the selected media backend, never by the session or transport. */
typedef struct vt_raw_frame {
    uint32_t struct_size;
    void* native_buffer;
    void (*retain)(void* native_buffer);
    void (*release)(void* native_buffer);
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint64_t frame_number;
    uint64_t pts_us;
} vt_raw_frame;

typedef struct vt_event {
    uint32_t struct_size;
    vt_event_type type;
    vt_status status;
} vt_event;

typedef struct vt_session_config {
    uint32_t struct_size;
    uint32_t abi_version;
    vt_role role;
    vt_input_mode input_mode;
    uint32_t max_frames;    /* Maximum queued frames per direction. */
    size_t max_bytes;       /* Maximum queued encoded/raw budget. */
    size_t max_frame_bytes; /* Maximum size of one encoded AU. */
    uint32_t max_age_ms;    /* Drop queued frames older than this. */
} vt_session_config;

typedef struct vt_callbacks {
    uint32_t struct_size;
    void* user;
    void (*on_event)(void* user, const vt_event* event);
    void (*on_encoded_frame)(void* user, const vt_encoded_frame* frame);
    void (*on_raw_frame)(void* user, const vt_raw_frame* frame);
} vt_callbacks;

struct vt_backends;

/* All callbacks are serialized by the session's dispatch thread. Register
 * them at create time. Do not call stop/destroy from a callback. A session is
 * one-shot: create a new session to restart after stop. */
VT_API vt_status vt_session_create(const vt_session_config* config, const vt_callbacks* callbacks,
    const struct vt_backends* backends, vt_session** out);
VT_API vt_status vt_session_start(vt_session* session);
VT_API vt_status vt_session_submit_encoded(vt_session* session, const vt_encoded_frame* frame);
VT_API vt_status vt_session_submit_raw(vt_session* session, const vt_raw_frame* frame);
/* Wait until all accepted outgoing frames have been handed to the transport.
 * A successful return does not acknowledge remote delivery. */
VT_API vt_status vt_session_drain(vt_session* session, uint32_t timeout_ms);
VT_API vt_status vt_session_get_state(const vt_session* session, vt_event* out);
VT_API vt_status vt_session_stop(vt_session* session);
VT_API void vt_session_destroy(vt_session* session);

/* Extension points for application-supplied transport, media and control
 * backends. The reference UDP transport below needs no custom backend code. */

/* Backend methods must not throw across this ABI. The session owns backend
 * contexts after create succeeds and calls destroy after stop. A backend may
 * invoke its sink from a worker thread; the session copies or retains the
 * frame before that invocation returns and dispatches application callbacks
 * on its own thread. A send/decode implementation must consume or copy AU
 * bytes before returning; a raw encoder can retain native storage using the
 * raw frame hooks. start implementations must copy their sink struct before
 * returning; its pointer is valid only for the duration of start. */
typedef struct vt_transport_sink {
    uint32_t struct_size;
    void* user;
    void (*on_frame)(void* user, const vt_encoded_frame* frame);
    void (*on_event)(void* user, vt_event_type type, vt_status status);
} vt_transport_sink;

typedef struct vt_transport_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    vt_status (*start)(void* context, vt_role role, const vt_transport_sink* sink);
    vt_status (*send)(void* context, const vt_encoded_frame* frame);
    void (*stop)(void* context);
    void (*destroy)(void* context);
} vt_transport_backend;

typedef struct vt_media_sink {
    uint32_t struct_size;
    void* user;
    void (*on_encoded)(void* user, const vt_encoded_frame* frame);
    void (*on_raw)(void* user, const vt_raw_frame* frame);
} vt_media_sink;

typedef struct vt_media_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    vt_status (*start)(void* context, vt_role role, const vt_media_sink* sink);
    vt_status (*encode)(void* context, const vt_raw_frame* frame);
    vt_status (*decode)(void* context, const vt_encoded_frame* frame);
    vt_status (*request_keyframe)(void* context);
    void (*stop)(void* context);
    void (*destroy)(void* context);
} vt_media_backend;

typedef enum vt_control_action {
    VT_CONTROL_REQUEST_KEYFRAME = 1,
    VT_CONTROL_SET_TARGET_BITRATE = 2,
    VT_CONTROL_QUERY_VIDEO = 3
} vt_control_action;

typedef struct vt_control_request {
    uint32_t struct_size;
    vt_control_action action;
    uint32_t target_bitrate_bps;
} vt_control_request;

typedef struct vt_control_sink {
    uint32_t struct_size;
    void* user;
    vt_status (*on_request)(void* user, const vt_control_request* request);
} vt_control_sink;

typedef struct vt_control_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    vt_status (*start)(void* context, vt_role role, const vt_control_sink* sink);
    vt_status (*send)(void* context, const vt_control_request* request);
    void (*stop)(void* context);
    void (*destroy)(void* context);
} vt_control_backend;

typedef struct vt_backends {
    uint32_t struct_size;
    const vt_transport_backend* transport; /* required */
    const vt_media_backend* media;         /* required for raw input or decoded output */
    const vt_control_backend* control;     /* optional */
} vt_backends;

VT_API vt_status vt_session_send_control(vt_session* session, const vt_control_request* request);

/* Reference UDP transport configuration. Addresses must be IPv4 literals;
 * the peer address may be empty for a receive-only ground session. */

typedef struct vt_reference_udp_config {
    uint32_t struct_size;
    const char* bind_address;
    const char* peer_address;
    uint16_t port;
    size_t max_frame_bytes;
} vt_reference_udp_config;

/* The resulting context is owned by the caller until vt_session_create
 * succeeds, then by the session. */
VT_API vt_status vt_reference_udp_create(const vt_reference_udp_config* config, vt_transport_backend* out);

/* Plugin and optional MPP adapters. These declarations do not require MPP headers. */

/* Optional plugin entry point, looked up as "vt_backend_plugin_create_v1".
 * Configuration is opaque to the caller and interpreted by the plugin.
 * On success transport owns a context. control is optional: an all-zero
 * control struct means absent. On failure both outputs are zero. The plugin
 * library must stay loaded until every returned backend context is destroyed. */
typedef vt_status (*vt_backend_plugin_create_v1_fn)(
    const char* configuration, vt_role role, vt_transport_backend* transport, vt_control_backend* control);

/* MPP types are optional: include <sys/vb_type.h> when using frame details. */
struct _VideoFrameInfo;

/* Wrap an MPP video frame as a vt_raw_frame. Decoder EOS frames are rejected. On success the caller owns one
 * reference, released by out->release(out->native_buffer). The callback is
 * called exactly once on the final release, potentially on another thread.
 * It must not throw and must keep its user context and MPP device alive until
 * then. On failure the caller retains all responsibility for the input. The
 * struct _VideoFrameInfo is copied; its underlying buffer is not copied. */
typedef void (*vt_mpp_release_fn)(void* user, const struct _VideoFrameInfo* info);
VT_API vt_status vt_mpp_frame_wrap(const struct _VideoFrameInfo* info, uint64_t frame_number, vt_mpp_release_fn release,
    void* user, vt_raw_frame* out);

/* Valid only while the caller holds a reference to this wrapped buffer. */
VT_API const struct _VideoFrameInfo* vt_mpp_frame_info(const vt_raw_frame* frame);

/* The application initializes SYS, VB, and UVC before opening, and must not
 * exit those modules until all returned frame references have been released.
 * open/get/close must be serialized by the caller. close stops new acquisitions;
 * device teardown is deferred until all outstanding frame references end. */
typedef struct vt_mpp_camera vt_mpp_camera;
typedef struct vt_mpp_camera_config {
    uint32_t struct_size;
    const char* device_path;
    int device_id;
    int channel_id;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t depth;
    uint32_t pixel_format; /* MppPixelFormat supported by this UVC device */
} vt_mpp_camera_config;
VT_API vt_status vt_mpp_camera_open(const vt_mpp_camera_config*, vt_mpp_camera** out);
/* On success caller owns one frame reference and must call its release hook.
 * MPP acquisition failures, including timeout, currently return VT_IO_ERROR. */
VT_API vt_status vt_mpp_camera_get(vt_mpp_camera*, int32_t timeout_ms, vt_raw_frame* out);
VT_API void vt_mpp_camera_close(vt_mpp_camera*);

/* Caller initializes SYS/VB and VENC (sky) or VDEC (ground) before use and
 * keeps them initialized until all returned raw frames are released. The
 * adapter creates and owns one exclusive codec channel. This baseline accepts
 * MPP-backed raw frames matching the configured geometry and pixel format;
 * it does not scale, crop, decode MJPEG camera input, or apply board fixes. */
typedef struct vt_mpp_media_config {
    uint32_t struct_size;
    uint32_t abi_version;
    vt_role role;
    int channel_id;
    uint32_t width, height, fps, bitrate, gop;
    uint32_t pixel_format; /* input for sky; output for ground */
} vt_mpp_media_config;
VT_API vt_status vt_mpp_media_create(const vt_mpp_media_config*, vt_media_backend* out);

/* SYS/VB/VDEC must be initialized by the application and remain initialized
 * until every returned frame is released. Calls on one decoder are serialized
 * by the caller. close defers channel teardown while output frames are held. */
typedef struct vt_mpp_mjpeg vt_mpp_mjpeg;
VT_API vt_status vt_mpp_mjpeg_open(int channel, uint32_t width, uint32_t height, vt_mpp_mjpeg** out);
/* Optional MPP decoder output scaling. Source and output sizes must be nonzero. */
VT_API vt_status vt_mpp_mjpeg_open_scaled(
    int channel, uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height, vt_mpp_mjpeg** out);
/* Synchronous one-frame decode. Caller owns one output reference on success.
 * After any decode failure, close and recreate the decoder before retrying. */
VT_API vt_status vt_mpp_mjpeg_decode(vt_mpp_mjpeg*, const vt_raw_frame* input, vt_raw_frame* out);
VT_API void vt_mpp_mjpeg_close(vt_mpp_mjpeg*);

/* Synchronously crop an NV12 MPP-backed input into an exclusively writable
 * NV12 MPP-backed output, scaling to its full dimensions. The caller owns both
 * buffers and ensures MPP/VB are initialized, the output is not used elsewhere,
 * and the buffers have compatible valid layouts and device access. Both frames
 * and their references must remain alive until return. This does not allocate
 * buffers, copy timestamps or perform platform-specific layout corrections. */
VT_API vt_status vt_mpp_crop_resize(
    const vt_raw_frame* input, const vt_raw_frame* output, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif  // VIDEO_TRANSMISSION_H
