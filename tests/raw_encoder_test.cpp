// Software MPP boundary test: interpose the used calls, never open hardware.
// Include the private Encoder without exporting a public testing API.
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sys_api.h>

#include <cassert>
#include <filesystem>  // NOLINT(build/c++17)
#include <map>
#include <vector>

// GCC warns about anonymous helper types only when their .cpp is included.
// They deliberately stay private; this translation unit exports no new API.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "../src/pipeline/session.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
using namespace video_transmission;  // NOLINT(build/namespaces)
std::map<UL, VideoFrameInfo> buffers;
VideoFrameInfo pool_layout{}, submitted{};
VencChnAttr encoder_attr{};
VencRcAttr rate_control{};
int rate_calls = 0, rate_result = 0;
std::deque<U64> output_pts;
int runtime_refs = 0, vb_refs = 0, sends = 0, pools = 0, blits = 0;
int send_result = 0, enable_result = 0, disables = 0, destroys = 0;
bool enabled = false;
std::vector<uint8_t> access_unit{0, 0, 1, 0x40, 1, 0, 0, 1, 0x26, 1};

VideoFrameInfo Layout(MppPixelFormat format, UL id, U64 pts = 0) {
    VideoFrameInfo v{};
    v.ulBufferId = id;
    v.eFrameType = FRAME_TYPE_COMMON;
    v.stCommFrameInfo.u32Width = 640;
    v.stCommFrameInfo.u32Height = 400;
    v.stCommFrameInfo.ePixelFormat = format;
    v.stCommFrameInfo.eCompressMode = COMPRESS_MODE_NONE;
    auto& b = v.stVFrame;
    b.u32PlaneNum = format == MPP_PIXEL_FORMAT_BGRA ? 1 : 2;
    b.u64PTS = pts;
    for (U32 p = 0; p < b.u32PlaneNum; ++p) {
        b.u32PlaneStride[p] = 640 * (format == MPP_PIXEL_FORMAT_BGRA ? 4 : 1);
        b.u32PlaneSize[p] = b.u32PlaneStride[p] * 400 / (p + 1);
        b.u32PlaneSizeValid[p] = b.u32PlaneSize[p];
        b.ulPlaneVirAddr[p] = 0x100000 + p * 0x100000;
        b.u64PlanePhyAddr[p] = b.ulPlaneVirAddr[p];
        b.u32Fd[p] = 7;
        b.u32TotalSize += b.u32PlaneSize[p];
    }
    return v;
}

MppFrame Frame(
    MppPixelFormat format, U64 pts, int* released = nullptr, bool require_destroyed = false) {
    const UL id = buffers.size() + 1;
    auto v = Layout(format, id, pts);
    buffers[id] = v;
    return AdoptMppFrame(v, [released, require_destroyed](const auto&) {
        if (require_destroyed) assert(!enabled && destroys > 0);
        if (released) ++*released;
    });
}

template <class F>
void Reject(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void Validation() {
    PipelineConfig c;
    auto bgra = Frame(MPP_PIXEL_FORMAT_BGRA, 1);
    auto nv12 = Frame(MPP_PIXEL_FORMAT_NV12, 2);
    ValidateRawFrame(bgra, c);
    ValidateRawFrame(nv12, c);
    auto bad = bgra;
    bad.owner.reset();
    Reject([&] { ValidateRawFrame(bad, c); });
    bad = bgra;
    bad.info.stVFrame.u32PlaneNum = 2;
    Reject([&] { ValidateRawFrame(bad, c); });
    bad = bgra;
    --bad.info.stVFrame.u32PlaneSizeValid[0];
    Reject([&] { ValidateRawFrame(bad, c); });
    bad = bgra;
    ++bad.info.stVFrame.u32Fd[0];
    Reject([&] { ValidateRawFrame(bad, c); });
    bad = bgra;
    bad.info.stCommFrameInfo.ePixelFormat = MPP_PIXEL_FORMAT_ABGR;
    Reject([&] { ValidateRawFrame(bad, c); });
    auto changed = c;
    changed.width = 320;
    Reject([&] { ValidateRawFrame(bgra, changed); });
    ValidateRawFrame(nv12, changed);  // NV12 resize remains supported.
    changed = c;
    changed.roi_x = 2;
    changed.roi_width = 638;
    changed.roi_height = 400;
    Reject([&] { ValidateRawFrame(bgra, changed); });
    ValidateRawFrame(nv12, changed);  // NV12 crop remains supported.

    // Padding is legal for a VB, but not for this direct codec contract.
    bad = Frame(MPP_PIXEL_FORMAT_BGRA, 3);
    auto& p = bad.info.stVFrame;
    p.u32PlaneStride[0] += 16;
    p.u32PlaneSize[0] = p.u32PlaneSizeValid[0] = p.u32TotalSize = p.u32PlaneStride[0] * 400;
    buffers[bad.info.ulBufferId] = bad.info;
    ValidateMppFrame(bad);
    Reject([&] { ValidateRawFrame(bad, c); });
    bad = Frame(MPP_PIXEL_FORMAT_BGRA, 4);
    bad.info.stCommFrameInfo.u32Width = 638;
    auto& b = bad.info.stVFrame;
    b.u32PlaneStride[0] = 638 * 4;
    b.u32PlaneSize[0] = b.u32PlaneSizeValid[0] = b.u32TotalSize = 638 * 4 * 400;
    buffers[bad.info.ulBufferId] = bad.info;
    changed = c;
    changed.width = 638;
    ValidateMppFrame(bad);
    Reject([&] { ValidateRawFrame(bad, changed); });
}

void DirectInput() {
    PipelineConfig c;
    c.max_frames = 1;
    int first_released = 0, second_released = 0, failed_released = 0, pending_released = 0;
    {
        Encoder encoder(c, MPP_PIXEL_FORMAT_BGRA);
        assert(encoder_attr.eInputPixelFormat == MPP_PIXEL_FORMAT_BGRA);
        assert(encoder_attr.u32MinQp == 10 && encoder_attr.u32MaxQp == 48);
        assert(!encoder.pool && pools == 0 && blits == 0);
        auto first = Frame(MPP_PIXEL_FORMAT_BGRA, 100, &first_released);
        const auto first_id = first.info.ulBufferId;
        encoder.Send(first);
        first = {};
        assert(submitted.ulBufferId == first_id && submitted.stVFrame.u64PTS == 100);
        assert(first_released == 0 && !encoder.Get().data);
        assert(first_released == 0);  // No output yet: do not unmap/reuse input.
        auto second = Frame(MPP_PIXEL_FORMAT_BGRA, 200, &second_released);
        encoder.Send(second);
        second = {};
        output_pts.push_back(999);  // Unmatched output must not retire an input.
        assert(encoder.Get().data && first_released == 0 && second_released == 0);
        output_pts.push_back(200);  // Release by PTS, not queue order.
        auto encoded = encoder.Get();
        assert(encoded.timestamp_us == 200 && encoded.parameter_sets && encoded.keyframe);
        assert(first_released == 0 && second_released == 1);
        Reject([&] { encoder.Send(Frame(MPP_PIXEL_FORMAT_BGRA, 100)); });
        Reject([&] { encoder.Send(Frame(MPP_PIXEL_FORMAT_NV12, 101)); });
        output_pts.push_back(100);
        assert(encoder.Get().data && first_released == 1);
        auto failed = Frame(MPP_PIXEL_FORMAT_BGRA, 300, &failed_released);
        send_result = -1;
        bool threw = false;
        try {
            encoder.Send(failed);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
        send_result = 0;
        failed = {};
        assert(failed_released == 1 && encoder.in_flight.empty());

        // Keep output-less frames until codec shutdown, with a fixed bound.
        const int sends_before = sends;
        for (U64 i = 0; i < c.max_frames + 3; ++i)
            encoder.Send(Frame(MPP_PIXEL_FORMAT_BGRA, 400 + i, &pending_released, true));
        int busy_released = 0;
        encoder.Send(Frame(MPP_PIXEL_FORMAT_BGRA, 500, &busy_released));
        assert(busy_released == 1 && pending_released == 0);
        assert(sends == sends_before + 4 && encoder.in_flight.size() == 4);
        assert(pools == 0 && blits == 0);
    }
    assert(disables == 1 && destroys == 1 && pending_released == 4);
    assert(runtime_refs == 0 && vb_refs == 0);
}

void Nv12Unchanged() {
    PipelineConfig c;
    const int old_blits = blits;
    {
        Encoder encoder(c, MPP_PIXEL_FORMAT_NV12);
        assert(encoder_attr.eInputPixelFormat == MPP_PIXEL_FORMAT_NV12 && encoder.pool);
        auto input = Frame(MPP_PIXEL_FORMAT_NV12, 600);
        encoder.Send(input);
        assert(blits == old_blits + 1 && submitted.ulBufferId != input.info.ulBufferId);
        assert(submitted.stVFrame.u64PTS == 600 && encoder.in_flight.empty());
    }
    assert(pools == 0 && runtime_refs == 0 && vb_refs == 0);
}

void InitializationFailure() {
    PipelineConfig c;
    enable_result = -1;
    const int old_destroys = destroys;
    bool threw = false;
    try {
        Encoder encoder(c, MPP_PIXEL_FORMAT_BGRA);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    enable_result = 0;
    assert(threw && destroys == old_destroys + 1);
    assert(runtime_refs == 0 && vb_refs == 0 && pools == 0);
}

void QpConfiguration() {
    PipelineConfig c;
    c.min_qp = 25;
    c.max_qp = 42;
    for (const auto format : {MPP_PIXEL_FORMAT_NV12, MPP_PIXEL_FORMAT_BGRA}) {
        Encoder encoder(c, format);
        assert(encoder_attr.u32MinQp == 25 && encoder_attr.u32MaxQp == 42);
        assert(encoder.SetBitrate(6000000) == 0);
        assert(rate_control.enRcMode == VENC_RC_MODE_CBR);
        assert(rate_control.u32BitRate == 6000000);
        assert(rate_control.u32MinQp == 25 && rate_control.u32MaxQp == 42);
        const int calls = rate_calls;
        assert(encoder.SetBitrate(0) != 0 && rate_calls == calls);
        rate_result = -7;
        assert(encoder.SetBitrate(5000000) == -7);
        rate_result = 0;
        assert(rate_control.u32MinQp == 25 && rate_control.u32MaxQp == 42);
    }
    assert(pools == 0 && runtime_refs == 0 && vb_refs == 0);
}

void SessionAdmission(const char* config_dir) {
    auto config = LoadConfig((std::filesystem::path(config_dir) / "sky.yaml").string());
    config.control_enabled = false;
    config.connections.sky.device_ip = "127.0.0.1";
    config.connections.ground.device_ip = "127.0.0.1";
    config.pipeline = PipelineConfig{};
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    assert(probe >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t len = sizeof(address);
    assert(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &len) == 0);
    config.link.port = ntohs(address.sin_port);
    close(probe);
    // Allocate mock descriptors before starting threads; the map is then read-only.
    auto bgra = Frame(MPP_PIXEL_FORMAT_BGRA, 700);
    auto nv12 = Frame(MPP_PIXEL_FORMAT_NV12, 701);
    auto wrong_generation = bgra;
    wrong_generation.generation = 999;
    {
        VideoTransmission session;
        assert(session.Start(config).ok());
        assert(runtime_refs == 0 && vb_refs == 0);  // No eager NV12 codec/pool.
        assert(session.SubmitFrame(wrong_generation) == SubmitResult::InvalidFrame);
        assert(session.SubmitFrame(bgra) == SubmitResult::Accepted);
        assert(session.SubmitFrame(nv12) == SubmitResult::InvalidFrame);
        auto update = session.UpdateConnections(config.connections).get();
        assert(update.ok());
        assert(session.SubmitFrame(bgra) == SubmitResult::Accepted);
        assert(session.SubmitFrame(nv12) == SubmitResult::InvalidFrame);
        session.Stop();
    }
    assert(runtime_refs == 0 && vb_refs == 0);
    {
        // A QueueFull frame must not choose the session's input pixel format.
        config.pipeline.max_bytes = nv12.info.stVFrame.u32TotalSize;
        VideoTransmission session;
        assert(session.Start(config).ok());
        assert(session.SubmitFrame(bgra) == SubmitResult::QueueFull);
        assert(session.SubmitFrame(nv12) == SubmitResult::Accepted);
        session.Stop();
    }
    assert(pools == 0 && runtime_refs == 0 && vb_refs == 0);
}
}  // namespace

// Interposition is local to this test executable, not the installed library.
extern "C" {
S32 SYS_Init() {
    ++runtime_refs;
    return 0;
}
S32 SYS_Exit() {
    --runtime_refs;
    return 0;
}
S32 VB_Init() {
    ++vb_refs;
    return 0;
}
S32 VB_Exit() {
    --vb_refs;
    return 0;
}
S32 VB_GetFrameInfo(UL id, VideoFrameInfo* value) {
    if (!buffers.count(id)) return -1;
    *value = buffers.at(id);
    return 0;
}
S32 VB_GetDmaBufFd(UL id, S32* fd) {
    if (!buffers.count(id)) return -1;
    *fd = 7;
    return 0;
}
S32 VB_GetPicBufferSize(VideoFrameInfo* value) {
    *value = Layout(value->stCommFrameInfo.ePixelFormat, 0);
    return value->stVFrame.u32TotalSize;
}
UL VB_CreatePool(VbPoolCfg*) {
    ++pools;
    return 90;
}
S32 VB_DestroyPool(UL) {
    --pools;
    return 0;
}
S32 VB_SetFrameInfo(UL, VideoFrameInfo* v) {
    pool_layout = *v;
    return 0;
}
UL VB_GetBuffer(UL, U32) {
    auto v = pool_layout;
    v.ulBufferId = 900;
    buffers[900] = v;
    return 900;
}
S32 VB_ReleaseBuffer(UL) { return 0; }
S32 VENC_Init() { return 0; }
S32 VENC_CreateChn(S32, const VencChnAttr* a) {
    encoder_attr = *a;
    return 0;
}
S32 VENC_EnableChn(S32) {
    if (!enable_result) enabled = true;
    return enable_result;
}
S32 VENC_DisableChn(S32) {
    enabled = false;
    ++disables;
    return 0;
}
S32 VENC_DestroyChn(S32) {
    ++destroys;
    return 0;
}
S32 VENC_SendFrame(S32, const VideoFrameInfo* v, U32) {
    ++sends;
    submitted = *v;
    return send_result;
}
S32 VENC_SetRateControl(S32, VencRcAttr* attr) {
    ++rate_calls;
    rate_control = *attr;
    return rate_result;
}
S32 VENC_GetStream(S32, StreamBufferInfo* s, U32) {
    if (output_pts.empty()) return -1;
    s->u64PTS = output_pts.front();
    output_pts.pop_front();
    s->pu8Addr = access_unit.data();
    s->u32Size = access_unit.size();
    s->bKeyFrame = MPP_TRUE;
    return 0;
}
S32 VENC_ReleaseStream(S32, const StreamBufferInfo*) { return 0; }
S32 V2D_BeginJob(V2DHandle* handle) {
    *handle = 1;
    return 0;
}
S32 V2D_AddBitblitTask(
    V2DHandle, const VideoFrameInfo*, V2DArea*, VideoFrameInfo*, V2DArea*, V2DCscMode) {
    ++blits;
    return 0;
}
S32 V2D_EndJob(V2DHandle) { return 0; }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    Validation();
    DirectInput();
    Nv12Unchanged();
    InitializationFailure();
    QpConfiguration();
    SessionAdmission(argv[1]);
    std::puts("raw_encoder_test: PASS (software MPP stubs; no hardware opened)");
}
