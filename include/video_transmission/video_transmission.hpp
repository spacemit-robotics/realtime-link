#ifndef VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_
#define VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_

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
    const uint8_t *data;
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
    void *native_buffer;
    void (*retain)(void *native_buffer);
    void (*release)(void *native_buffer);
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
    uint32_t max_frames;      /* Maximum queued frames per direction. */
    size_t max_bytes;         /* Maximum queued encoded/raw budget. */
    size_t max_frame_bytes;   /* Maximum size of one encoded AU. */
    uint32_t max_age_ms;      /* Drop queued frames older than this. */
} vt_session_config;

typedef struct vt_callbacks {
    uint32_t struct_size;
    void *user;
    void (*on_event)(void *user, const vt_event *event);
    void (*on_encoded_frame)(void *user, const vt_encoded_frame *frame);
    void (*on_raw_frame)(void *user, const vt_raw_frame *frame);
} vt_callbacks;

struct vt_backends;

/* All callbacks are serialized by the session's dispatch thread. Register
 * them at create time. Do not call stop/destroy from a callback. A session is
 * one-shot: create a new session to restart after stop. */
VT_API vt_status vt_session_create(const vt_session_config *config, const vt_callbacks *callbacks,
                                   const struct vt_backends *backends, vt_session **out);
VT_API vt_status vt_session_start(vt_session *session);
VT_API vt_status vt_session_submit_encoded(vt_session *session, const vt_encoded_frame *frame);
VT_API vt_status vt_session_submit_raw(vt_session *session, const vt_raw_frame *frame);
/* Wait until all accepted outgoing frames have been handed to the transport.
 * A successful return does not acknowledge remote delivery. */
VT_API vt_status vt_session_drain(vt_session *session, uint32_t timeout_ms);
VT_API vt_status vt_session_get_state(const vt_session *session, vt_event *out);
VT_API vt_status vt_session_stop(vt_session *session);
VT_API void vt_session_destroy(vt_session *session);

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
    void *user;
    void (*on_frame)(void *user, const vt_encoded_frame *frame);
    void (*on_event)(void *user, vt_event_type type, vt_status status);
} vt_transport_sink;

typedef struct vt_transport_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    vt_status (*start)(void *context, vt_role role, const vt_transport_sink *sink);
    vt_status (*send)(void *context, const vt_encoded_frame *frame);
    void (*stop)(void *context);
    void (*destroy)(void *context);
} vt_transport_backend;

typedef struct vt_media_sink {
    uint32_t struct_size;
    void *user;
    void (*on_encoded)(void *user, const vt_encoded_frame *frame);
    void (*on_raw)(void *user, const vt_raw_frame *frame);
} vt_media_sink;

typedef struct vt_media_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    vt_status (*start)(void *context, vt_role role, const vt_media_sink *sink);
    vt_status (*encode)(void *context, const vt_raw_frame *frame);
    vt_status (*decode)(void *context, const vt_encoded_frame *frame);
    vt_status (*request_keyframe)(void *context);
    void (*stop)(void *context);
    void (*destroy)(void *context);
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
    void *user;
    vt_status (*on_request)(void *user, const vt_control_request *request);
} vt_control_sink;

typedef struct vt_control_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    vt_status (*start)(void *context, vt_role role, const vt_control_sink *sink);
    vt_status (*send)(void *context, const vt_control_request *request);
    void (*stop)(void *context);
    void (*destroy)(void *context);
} vt_control_backend;

typedef struct vt_backends {
    uint32_t struct_size;
    const vt_transport_backend *transport; /* required */
    const vt_media_backend *media;         /* required for raw input or decoded output */
    const vt_control_backend *control;     /* optional */
} vt_backends;

VT_API vt_status vt_session_send_control(vt_session *session, const vt_control_request *request);

/* Reference UDP transport configuration. Addresses must be IPv4 literals;
 * the peer address may be empty for a receive-only ground session. */

/* Public reference protocol: IPv4 UDP, version 1. Each datagram has a
 * 36-byte network-byte-order header followed by at most 1120 AU bytes.
 * It is independent of the product UDP/USB wire formats. */
typedef struct vt_reference_udp_config {
    uint32_t struct_size;
    const char *bind_address;
    const char *peer_address;
    uint16_t port;
    size_t max_frame_bytes;
} vt_reference_udp_config;

/* The resulting context is owned by the caller until vt_session_create
 * succeeds, then by the session. */
VT_API vt_status vt_reference_udp_create(const vt_reference_udp_config *config,
                                         vt_transport_backend *out);

#ifdef __cplusplus
}
#endif

#endif  // VIDEO_TRANSMISSION_VIDEO_TRANSMISSION_HPP_
