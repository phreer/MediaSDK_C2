/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017-2019 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#include "mfx_c2_defs.h"
#include <gtest/gtest.h>
#include "test_components.h"
#include "mfx_c2_utils.h"
#include "mfx_c2_component.h"
#include "mfx_c2_components_registry.h"
#include "test_params.h"
#include "C2PlatformSupport.h"
#include "streams/h264/stream_nv12_176x144_cqp_g30_100.264.h"
#include "streams/h264/stream_nv12_352x288_cqp_g15_100.264.h"
#include "streams/h265/stream_nv12_176x144_cqp_g30_100.265.h"
#include "streams/h265/stream_nv12_352x288_cqp_g15_100.265.h"
#include "streams/vp9/stream_nv12_176x144_cqp_g30_100.vp9.ivf.h"
#include "streams/vp9/stream_nv12_352x288_cqp_g15_100.vp9.ivf.h"
#include "util/C2Debug-base.h"

#include <future>
#include <set>
#include <queue>

using namespace android;

const uint64_t FRAME_DURATION_US = 33333; // 30 fps
const c2_nsecs_t TIMEOUT_NS = MFX_SECOND_NS;

static std::vector<C2ParamDescriptor> dec_params_desc =
{
    { false, "MemoryType", C2MemoryTypeSetting::PARAM_TYPE },
};

namespace {
    struct ComponentDesc
    {
        const char* component_name;
        int flags;
        c2_status_t creation_status;
        std::vector<C2ParamDescriptor> params_desc;
        std::vector<std::vector<const StreamDescription*>> streams;
    };

    struct StreamChunk
    {
        StreamDescription::Region region{};
        bool header{};
        bool complete_frame{};
        bool valid{};
        bool flush{};
        bool end_stream{};
    };

    // Particularities how to run decoding, they may include:
    // 1) How many times repeat decoding.
    // 2) What decoders or streams should be skipped from test.
    // 3) How data chunks supplied to decoder are changed relative to
    // complete frames.
    // See examples in g_decoding_conditions initialization.
    struct DecodingConditions
    {
        const char* name;
        int repeat_count{1};
        std::function<bool(
            const std::vector<const StreamDescription*>& streams,
            const ComponentDesc& component_desc)> skip;
        std::function<void(
            const std::vector<const StreamDescription*>& streams,
            std::list<StreamChunk>* chunks)> chunks_mutator;
    };

    // These functions are used by ::testing::PrintToStringParamName to give
    // parameterized tests reasonable names instead of /1, /2, ...
    void PrintTo(const ComponentDesc& desc, ::std::ostream* os)
    {
        PrintAlphaNumeric(desc.component_name, os);
    }

    void PrintTo(const std::tuple<DecodingConditions, ComponentDesc>& tuple, ::std::ostream* os)
    {
        *os << std::get<DecodingConditions>(tuple).name << "_";
        PrintTo(std::get<ComponentDesc>(tuple), os);
    }

    class CreateDecoder : public ::testing::TestWithParam<ComponentDesc>
    {
    };
    // Test fixture class called Decoder to beautify output
    class Decoder : public ::testing::TestWithParam<ComponentDesc>
    {
    };

    class DecoderDecode : public ::testing::TestWithParam<std::tuple<DecodingConditions, ComponentDesc>>
    {
    };
}

static std::vector<std::vector<const StreamDescription*>> h264_streams =
{
    { &stream_nv12_176x144_cqp_g30_100_264 },
    { &stream_nv12_352x288_cqp_g15_100_264 },
    { &stream_nv12_176x144_cqp_g30_100_264, &stream_nv12_352x288_cqp_g15_100_264 },
    { &stream_nv12_352x288_cqp_g15_100_264, &stream_nv12_176x144_cqp_g30_100_264 }
};

static std::vector<std::vector<const StreamDescription*>> h265_streams =
{
    { &stream_nv12_176x144_cqp_g30_100_265 },
    { &stream_nv12_352x288_cqp_g15_100_265 },
    { &stream_nv12_176x144_cqp_g30_100_265, &stream_nv12_352x288_cqp_g15_100_265 },
    { &stream_nv12_352x288_cqp_g15_100_265, &stream_nv12_176x144_cqp_g30_100_265 }
};

static std::vector<std::vector<const StreamDescription*>> vp9_streams =
{
    { &stream_nv12_176x144_cqp_g30_100_vp9_ivf },
    { &stream_nv12_352x288_cqp_g15_100_vp9_ivf },
    { &stream_nv12_176x144_cqp_g30_100_vp9_ivf, &stream_nv12_352x288_cqp_g15_100_vp9_ivf },
    { &stream_nv12_352x288_cqp_g15_100_vp9_ivf, &stream_nv12_176x144_cqp_g30_100_vp9_ivf }
};

static ComponentDesc g_components_desc[] = {
    { "C2.h264vd", 0, C2_OK, dec_params_desc, h264_streams },
    { "C2.h265vd", 0, C2_OK, dec_params_desc, h265_streams },
    { "C2.vp9vd",  0, C2_OK, dec_params_desc, vp9_streams },
};

static ComponentDesc g_invalid_components_desc[] = {
    { "C2.NonExistingDecoder", 0, C2_NOT_FOUND, {}, {} },
};

static std::list<StreamChunk> ReadChunks(const std::vector<const StreamDescription*>& streams)
{
    std::unique_ptr<StreamReader> reader{StreamReader::Create(streams)};
    std::list<StreamChunk> chunks;
    StreamChunk chunk;

    while (reader->Read(StreamReader::Slicing::Frame(), &chunk.region, &chunk.header)) {
        chunk.complete_frame = true;
        chunk.valid = true;
        chunk.flush = false;
        chunk.end_stream = reader->EndOfStream();
        chunks.emplace_back(std::move(chunk));
    }
    return chunks;
}

// Inserts part of header before the header,
// for header in the middle of the stream (resolution change).
static void InsertHeaderPart(const std::vector<const StreamDescription*>& streams,
    std::list<StreamChunk>* chunks)
{
    std::unique_ptr<StreamReader> reader{StreamReader::Create(streams)};
    for (auto it = chunks->begin(); it != chunks->end(); ++it) {
        if (it->header && it != chunks->begin()) {
            StreamChunk chunk(*it); // make a copy
            const size_t HEADER_PART_SIZE = 5;
            EXPECT_GT(chunk.region.size, HEADER_PART_SIZE);
            if (chunk.region.size > HEADER_PART_SIZE) {
                chunk.region.size = HEADER_PART_SIZE; // part of header
            }
            chunk.complete_frame = false;
            chunks->insert(it, chunk);
        }
    }
}

// Split header by NAL units.
static void SplitHeaders(const std::vector<const StreamDescription*>& streams,
    std::list<StreamChunk>* chunks)
{
    std::unique_ptr<StreamReader> reader{StreamReader::Create(streams)};
    for (auto it = chunks->begin(); it != chunks->end(); ) {
        if (it->header) {
            size_t offset = it->region.offset;
            ssize_t left = it->region.size;
            // replace with split
            reader->Seek(offset);
            while (left > 0) {
                StreamChunk chunk;
                bool res = reader->Read(StreamReader::Slicing::NalUnit(), &chunk.region, &chunk.header);
                if (!res) break;
                left -= chunk.region.size;
                chunk.complete_frame = (left <= 0);
                chunk.valid = true;
                chunk.flush = false;
                chunk.end_stream = reader->EndOfStream();
                chunks->insert(it, chunk);
            }
            // erase
            it = chunks->erase(it);
        } else {
            ++it;
        }
    }
}

// Cuts eos to a separate chunk.
static void CutEos(const std::vector<const StreamDescription*>&/* streams*/, std::list<StreamChunk>* chunks)
{
    auto it = chunks->rbegin();
    StreamChunk chunk(*it); // copy
    chunk.region.offset += chunk.region.size;
    chunk.region.size = 0;
    chunk.complete_frame = false;


    it->end_stream = false; // not an eos anymore

    chunks->push_back(chunk);
}

// Cuts eos and appends series of eos chunks.
static void AppendMultipleEos(const std::vector<const StreamDescription*>&/* streams*/,
    std::list<StreamChunk>* chunks)
{
    auto it = chunks->rbegin();
    StreamChunk chunk(*it); // copy
    it->end_stream = false; // not an eos anymore

    chunk.region.offset += chunk.region.size;
    chunk.region.size = 0;
    chunk.complete_frame = false;
    chunks->push_back(chunk);

    const int EXCESSIVE_EOS_COUNT = 9;
    for (int i = 0; i < EXCESSIVE_EOS_COUNT; ++i) {
        chunk.complete_frame = false;
        chunk.valid = false;
        chunk.flush = true;
        chunks->push_back(chunk);
    }
}

static std::vector<DecodingConditions> g_decoding_conditions = []() {
    std::vector<DecodingConditions> res;

    res.push_back({});
    res.back().name = "DecodeBitExact";
    const int BIT_EXACT_REPEAT_COUNT = 3;
    res.back().repeat_count = BIT_EXACT_REPEAT_COUNT;

    // Decodes streams that caused resolution change,
    // supply part of second header, it caused undefined behaviour in mediasdk decoder (264)
    // then supply completed header, expects decoder recovers and decodes stream fine.
    res.push_back({});
    res.back().name = "BrokenHeader";
    res.back().skip = [](const std::vector<const StreamDescription*>& streams, const ComponentDesc&) {
        return streams.size() == 1;
    };
    res.back().chunks_mutator = InsertHeaderPart;

    // Sends streams for decoding emulating C2 runtime behaviour:
    // if frame contains header, the frame is sent split by NAL units.
    res.push_back({});
    res.back().name = "SeparateHeaders";
    res.back().skip = [](const std::vector<const StreamDescription*>&, const ComponentDesc& desc) {
        return std::string(desc.component_name) == "C2.vp9vd";
    };
    res.back().chunks_mutator = SplitHeaders;

    // Sends last frame without eos flag, then empty input buffer with eos flag.
    res.push_back({});
    res.back().name = "SeparateEos";
    res.back().chunks_mutator = CutEos;

    // Follow last frame with series of Eos works without frame.
    res.push_back({});
    res.back().name = "MultipleEos";
    res.back().chunks_mutator = AppendMultipleEos;

    return res;
}();

// Assures that all decoding components might be successfully created.
// NonExistingDecoder cannot be created and C2_NOT_FOUND error is returned.
TEST_P(CreateDecoder, Create)
{
    const ComponentDesc& desc = GetParam();

    std::shared_ptr<MfxC2Component> decoder = GetCachedComponent(desc);

    EXPECT_EQ(decoder != nullptr, desc.creation_status == C2_OK) << " for " << desc.component_name;
}

// Checks that all successfully created decoding components expose C2ComponentInterface
// and return correct information once queried (component name).
TEST_P(Decoder, intf)
{
    CallComponentTest<ComponentDesc>(GetParam(),
        [] (const ComponentDesc& desc, C2CompPtr, C2CompIntfPtr comp_intf) {

        EXPECT_EQ(comp_intf->getName(), desc.component_name);
    } );
}

// Checks list of actually supported parameters by all decoding components.
// Parameters order doesn't matter.
// For every parameter index, name, required and persistent fields are checked.
TEST_P(Decoder, getSupportedParams)
{
    CallComponentTest<ComponentDesc>(GetParam(),
        [] (const ComponentDesc& desc, C2CompPtr, C2CompIntfPtr comp_intf) {

        std::vector<std::shared_ptr<C2ParamDescriptor>> params_actual;
        c2_status_t sts = comp_intf->querySupportedParams_nb(&params_actual);
        EXPECT_EQ(sts, C2_OK);

        EXPECT_EQ(desc.params_desc.size(), params_actual.size());

        for(const C2ParamDescriptor& param_expected : desc.params_desc) {

            const auto found_actual = std::find_if(params_actual.begin(), params_actual.end(),
                [&] (auto p) { return p->index() == param_expected.index(); } );

            EXPECT_NE(found_actual, params_actual.end())
                << "missing parameter " << param_expected.name();
            if (found_actual != params_actual.end()) {
                EXPECT_EQ((*found_actual)->isRequired(), param_expected.isRequired());
                EXPECT_EQ((*found_actual)->isPersistent(), param_expected.isPersistent());
                EXPECT_EQ((*found_actual)->name(), param_expected.name());
            }
        }
    } );
}

static void PrepareWork(uint32_t frame_index,
    std::shared_ptr<const C2Component> component,
    std::unique_ptr<C2Work>* work,
    const std::vector<char>& bitstream, bool end_stream, bool header, bool complete)
{
    *work = std::make_unique<C2Work>();
    C2FrameData* buffer_pack = &((*work)->input);

    buffer_pack->flags = C2FrameData::flags_t(0);
    // If complete frame do not set C2FrameData::FLAG_CODEC_CONFIG regardless header parameter
    // as it is set when buffer contains header only.
    if (!complete) {
        if (header) {
            buffer_pack->flags = C2FrameData::flags_t(buffer_pack->flags | C2FrameData::FLAG_CODEC_CONFIG);
        } else {
            buffer_pack->flags = C2FrameData::flags_t(buffer_pack->flags | C2FrameData::FLAG_INCOMPLETE);
        }
    }
    if (end_stream)
        buffer_pack->flags = C2FrameData::flags_t(buffer_pack->flags | C2FrameData::FLAG_END_OF_STREAM);

    // Set up frame header properties:
    // timestamp is set to correspond to 30 fps stream.
    buffer_pack->ordinal.timestamp = FRAME_DURATION_US * frame_index;
    buffer_pack->ordinal.frameIndex = frame_index;
    buffer_pack->ordinal.customOrdinal = 0;

    do {

        if (!bitstream.empty()) {
            std::shared_ptr<C2BlockPool> allocator;
            c2_status_t sts = GetCodec2BlockPool(C2BlockPool::BASIC_LINEAR,
                component, &allocator);

            EXPECT_EQ(sts, C2_OK);
            EXPECT_NE(allocator, nullptr);

            if(nullptr == allocator) break;

            C2MemoryUsage mem_usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
            std::shared_ptr<C2LinearBlock> block;
            sts = allocator->fetchLinearBlock(bitstream.size(),
                mem_usage, &block);

            EXPECT_EQ(sts, C2_OK);
            EXPECT_NE(block, nullptr);

            if(nullptr == block) break;

            std::unique_ptr<C2WriteView> write_view;
            sts = MapLinearBlock(*block, TIMEOUT_NS, &write_view);
            EXPECT_EQ(sts, C2_OK);
            EXPECT_NE(write_view, nullptr);

            uint8_t* data = write_view->data();
            EXPECT_NE(data, nullptr);

            std::copy(bitstream.begin(), bitstream.end(), data);

            C2Event event;
            event.fire(); // pre-fire as buffer is already ready to use
            C2ConstLinearBlock const_block = block->share(0, bitstream.size(), event.fence());
            // make buffer of linear block
            std::shared_ptr<C2Buffer> buffer = std::make_shared<C2Buffer>(MakeC2Buffer( { const_block } ));

            buffer_pack->buffers.push_back(buffer);
        }

        std::unique_ptr<C2Worklet> worklet = std::make_unique<C2Worklet>();
        // work of 1 worklet
        (*work)->worklets.push_back(std::move(worklet));
    } while(false);
}

class Expectation
{
public:
    std::string Format()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream ss;
        bool any_printed = false;

        auto print_set = [&](const char* name, const std::set<uint64_t>& set) {

            if (!set.empty()) {
                if (any_printed) ss << "; ";
                ss << name;
                bool first = true;
                for (const auto& index : set) {
                    ss << (first ? ": " : ",") << index;
                    first = false;
                }
                any_printed = true;
            }
        };

        print_set("Filled", frame_set_);
        print_set("Empty", frame_empty_set_);

        if (!failures_.empty()) {
            if (any_printed) ss << "; ";
            ss << "Failures: ";
            bool first = true;
            for (auto const& pair : failures_) {
                ss << (first ? "{ " : ", ") << pair.first << ":" << pair.second;
                first = false;
            }
            ss << " }";
        }

        return ss.str();
    }

    void ExpectFrame(uint64_t frame_index, bool expect_empty)
    {
        MFX_DEBUG_TRACE_FUNC;
        std::lock_guard<std::mutex> lock(mutex_);
        if (expect_empty) {
            MFX_DEBUG_TRACE_STREAM("Register empty: " << frame_index);
            frame_empty_set_.insert(frame_index);
        } else {
            MFX_DEBUG_TRACE_STREAM("Register filled: " << frame_index);
            frame_set_.insert(frame_index);
        }
    }

    void ExpectFailures(int count, c2_status_t status)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_[status] += count;
    }
    // Assign *met_all true if passed frame_index completes all expectations.
    void CheckFrame(uint64_t frame_index, bool frame_empty, bool* met_all = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<uint64_t>& check_set = frame_empty ? frame_empty_set_ : frame_set_;
        size_t erased_count = check_set.erase(frame_index);
        EXPECT_EQ(erased_count, 1u) <<
            "unexpected " << (frame_empty ? "empty" : "filled") << " frame #" << frame_index;
        // This method is used to signal completion of all expectations with std::promise,
        // so met_all should be returned as true only once
        // and under same mutex as expected sets modifications to avoid call double std::promise::set_value.
        if (met_all) {
            *met_all = (erased_count > 0) && EmptyInternal();
        }
    }
    // Assign *met_all true if passed expected error completes all expectations.
    void CheckFailure(c2_status_t status, bool* met_all = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool expected = failures_[status] > 0;
        EXPECT_TRUE(expected);
        failures_[status]--;
        if (failures_[status] == 0) {
            failures_.erase(status);
        }
        if (met_all) {
            *met_all = expected && EmptyInternal();
        }
    }

    bool Empty()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return EmptyInternal();
    }

private:
    bool EmptyInternal() // private thread-unsafe version
    {
        return frame_set_.empty() && frame_empty_set_.empty() &&
            failures_.empty();
    }

private:
    std::mutex mutex_;
    std::set<uint64_t> frame_set_;
    std::set<uint64_t> frame_empty_set_;
    std::map<c2_status_t, int> failures_; // expected failures and how many times they should occur
};

class DecoderConsumer : public C2Component::Listener
{
public:
    typedef std::function<void(uint32_t width, uint32_t height,
        const uint8_t* data, size_t length)> OnFrame;

public:
    DecoderConsumer(OnFrame on_frame):
        on_frame_(on_frame) {}

    // future ready when validator got all expectations (frames and failures)
    std::future<void> GetFuture()
    {
        return done_.get_future();
    }

    Expectation& GetExpectation()
    {
        return expect_;
    }

protected:
    void onWorkDone_nb(
        std::weak_ptr<C2Component> component,
        std::list<std::unique_ptr<C2Work>> workItems) override
    {
        for(std::unique_ptr<C2Work>& work : workItems) {
            EXPECT_EQ(work->worklets.size(), 1u);
            if (C2_OK == work->result) {
                EXPECT_EQ(work->workletsProcessed, 1u);
                if (work->worklets.size() >= 1) {

                    std::unique_ptr<C2Worklet>& worklet = work->worklets.front();
                    C2FrameData& buffer_pack = worklet->output;

                    uint64_t frame_index = buffer_pack.ordinal.frameIndex.peeku();

                    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
                    EXPECT_EQ(buffer_pack.flags, eos ? C2FrameData::FLAG_END_OF_STREAM : C2FrameData::flags_t{});
                    EXPECT_EQ(buffer_pack.ordinal.timestamp, frame_index * FRAME_DURATION_US); // 30 fps

                    c2_status_t sts{};
                    std::unique_ptr<C2ConstGraphicBlock> graphic_block;

                    if (buffer_pack.buffers.size() > 0) {
                        sts = GetC2ConstGraphicBlock(buffer_pack, &graphic_block);
                        EXPECT_EQ(sts, C2_OK) << NAMED(frame_index) << "Output buffer count: " << buffer_pack.buffers.size();
                    }
                    if(nullptr != graphic_block) {

                        C2Rect crop = graphic_block->crop();
                        EXPECT_NE(crop.width, 0u);
                        EXPECT_NE(crop.height, 0u);

                        std::shared_ptr<C2Component> comp = component.lock();
                        if (comp) {
                            C2StreamPictureSizeInfo::output size_info;
                            C2StreamCropRectInfo::output crop_info;
                            sts = comp->intf()->query_vb({&size_info, &crop_info}, {}, C2_MAY_BLOCK, nullptr);
                            EXPECT_EQ(sts, C2_OK);
                            EXPECT_EQ(size_info.width, graphic_block->width());
                            EXPECT_EQ(size_info.height, graphic_block->height());

                            EXPECT_EQ((C2Rect)crop_info, crop);

                            comp = nullptr;
                        }

                        std::unique_ptr<const C2GraphicView> c_graph_view;
                        sts = MapConstGraphicBlock(*graphic_block, TIMEOUT_NS, &c_graph_view);
                        EXPECT_EQ(sts, C2_OK) << NAMED(sts);
                        EXPECT_TRUE(c_graph_view);

                        if (c_graph_view) {
                            C2PlanarLayout layout = c_graph_view->layout();

                            const uint8_t* const* raw  = c_graph_view->data();

                            EXPECT_NE(raw, nullptr);
                            for (uint32_t i = 0; i < layout.numPlanes; ++i) {
                                EXPECT_NE(raw[i], nullptr);
                            }

                            std::shared_ptr<std::vector<uint8_t>> data_buffer = std::make_shared<std::vector<uint8_t>>();
                            data_buffer->resize(crop.width * crop.height * 3 / 2);
                            uint8_t* raw_cropped = &(data_buffer->front());
                            uint8_t* raw_cropped_chroma = raw_cropped + crop.width * crop.height;
                            const uint8_t* raw_chroma = raw[C2PlanarLayout::PLANE_U];

                            for (uint32_t i = 0; i < crop.height; i++) {
                                const uint32_t stride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
                                const uint8_t* row = raw[C2PlanarLayout::PLANE_Y] + (i + crop.top) * stride + crop.left;
                                std::copy(row, row + crop.width, raw_cropped + i * crop.width);
                            }
                            for (uint32_t i = 0; i < (crop.height >> 1); i++) {
                                const uint32_t stride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
                                const uint8_t* row = raw_chroma + (i + (crop.top >> 1)) * stride + crop.left;
                                std::copy(row, row + crop.width, raw_cropped_chroma + i * crop.width);
                            }

                            if(nullptr != raw_cropped) {
                                on_frame_(crop.width, crop.height,
                                    raw_cropped, crop.width * crop.height * 3 / 2);
                            }
                        }
                    }
                    bool empty = buffer_pack.buffers.size() == 0;
                    bool expectations_met = false;
                    expect_.CheckFrame(frame_index, empty, &expectations_met);
                    if (expectations_met) {
                        done_.set_value();
                    }
                    if (empty && eos) { // Separate eos work should be last of expected.
                        EXPECT_TRUE(expectations_met) << NAMED(frame_index) << " left: " << expect_.Format();
                    }
                }
            } else {
                EXPECT_EQ(work->workletsProcessed, 0u);
                bool expectations_met = false;
                expect_.CheckFailure(work->result, &expectations_met);
                if (expectations_met) {
                    done_.set_value();
                }
            }
        }
    }

    void onTripped_nb(std::weak_ptr<C2Component> component,
                           std::vector<std::shared_ptr<C2SettingResult>> settingResult) override
    {
        (void)component;
        (void)settingResult;
        EXPECT_EQ(true, false) << "onTripped_nb callback shouldn't come";
    }

    void onError_nb(std::weak_ptr<C2Component> component,
                         uint32_t errorCode) override
    {
        (void)component;
        (void)errorCode;
        EXPECT_EQ(true, false) << "onError_nb callback shouldn't come";
    }

private:
    OnFrame on_frame_;
    Expectation expect_;
    std::promise<void> done_;  // fire when all expected frames came
};

static void Decode(
    bool graphics_memory,
    std::shared_ptr<C2Component> component,
    std::shared_ptr<DecoderConsumer> validator,
    const std::vector<const StreamDescription*>& streams,
    const std::list<StreamChunk>& stream_chunks)
{
    c2_blocking_t may_block{C2_MAY_BLOCK};
    component->setListener_vb(validator, may_block);

    C2MemoryTypeSetting setting;
    setting.value = graphics_memory ? C2MemoryTypeGraphics : C2MemoryTypeSystem;

    std::vector<C2Param*> params = { &setting };
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    std::shared_ptr<C2ComponentInterface> comp_intf = component->intf();

    c2_status_t sts = comp_intf->config_vb(params, may_block, &failures);
    EXPECT_EQ(sts, C2_OK);

    sts = component->start();
    EXPECT_EQ(sts, C2_OK);

    uint32_t frame_index = 0;

    Expectation flush_expect;
    std::unique_ptr<StreamReader> reader{StreamReader::Create(streams)};

    for (const StreamChunk& chunk : stream_chunks) {

        std::vector<char> stream_part = reader->GetRegionContents(chunk.region);

        // prepare worklet and push
        std::unique_ptr<C2Work> work;

        // insert input data
        PrepareWork(frame_index, component, &work, stream_part,
            chunk.end_stream, chunk.header, chunk.complete_frame);
        std::list<std::unique_ptr<C2Work>> works;
        works.push_back(std::move(work));

        Expectation& expect = chunk.flush ? flush_expect : validator->GetExpectation();
        if (chunk.valid) {
            expect.ExpectFrame(frame_index, !chunk.complete_frame);
        } else {
            expect.ExpectFailures(1, C2_BAD_VALUE);
        }

        sts = component->queue_nb(&works);
        EXPECT_EQ(sts, C2_OK);

        frame_index++;
    }

    std::future<void> future = validator->GetFuture();
    std::future_status future_sts = future.wait_for(std::chrono::seconds(10));

    EXPECT_EQ(future_sts, std::future_status::ready) << " failed expectations: "
        << validator->GetExpectation().Format();

    std::list<std::unique_ptr<C2Work>> flushed_work;
    sts = component->flush_sm(C2Component::FLUSH_COMPONENT, &flushed_work);
    EXPECT_EQ(sts, C2_OK);

    while (!flushed_work.empty()) {
        std::unique_ptr<C2Work> work = std::move(flushed_work.front());
        flushed_work.pop_front();

        if (C2_OK == work->result) {
            for (std::unique_ptr<C2Worklet>& worklet : work->worklets) {
                bool empty = (worklet->output.buffers.size() == 0);
                flush_expect.CheckFrame(worklet->output.ordinal.frameIndex.peeku(), empty);
            }
        } else {
            flush_expect.CheckFailure(work->result);
        }
    }
    EXPECT_TRUE(flush_expect.Empty()) << "Failed expectations: " << flush_expect.Format();

    component->setListener_vb(nullptr, may_block);
    sts = component->stop();
    EXPECT_EQ(sts, C2_OK);
}

static std::string GetStreamsCombinedName(const std::vector<const StreamDescription*>& streams)
{
    std::ostringstream res;

    bool first = true;
    for (const auto& stream : streams) {
        if (!first) {
            res << "-";
        }
        res << stream->name;
        first = false;
    }
    return res.str();
}

// Checks the correctness of all decoding components state machine.
// The component should be able to start from STOPPED (initial) state,
// stop from RUNNING state. Otherwise, C2_BAD_STATE should be returned.
TEST_P(Decoder, State)
{
    CallComponentTest<ComponentDesc>(GetParam(),
        [this] (const ComponentDesc&, C2CompPtr comp, C2CompIntfPtr) {

        c2_status_t sts = C2_OK;

        sts = comp->start();
        EXPECT_EQ(sts, C2_OK);

        sts = comp->start();
        EXPECT_EQ(sts, C2_BAD_STATE);

        sts = comp->stop();
        EXPECT_EQ(sts, C2_OK);

        sts = comp->stop();
        EXPECT_EQ(sts, C2_BAD_STATE);

        sts = comp->release();
        EXPECT_EQ(sts, C2_OK);

        sts = comp->release();
        EXPECT_EQ(sts, C2_DUPLICATE);
         // Re-create the component.
        ComponentsCache::GetInstance()->RemoveComponent(GetParam().component_name);
        comp = GetCachedComponent(GetParam());

        sts = comp->start();
        EXPECT_EQ(sts, C2_OK);

        sts = comp->release();
        EXPECT_EQ(sts, C2_OK);
        // Remove from cache as released component is not reusable.
        ComponentsCache::GetInstance()->RemoveComponent(GetParam().component_name);
    } );
}

static C2ParamValues GetConstParamValues()
{
    C2ParamValues const_values;

    const_values.Append(new C2ComponentDomainSetting(C2Component::DOMAIN_VIDEO));
    const_values.Append(new C2ComponentKindSetting(C2Component::KIND_DECODER));
    const_values.Append(new C2StreamFormatConfig::input(0/*stream*/, C2FormatCompressed));
    const_values.Append(new C2StreamFormatConfig::output(0/*stream*/, C2FormatVideo));
    return const_values;
}

// Queries constant platform parameters values and checks expectations.
TEST_P(Decoder, ComponentConstParams)
{
    CallComponentTest<ComponentDesc>(GetParam(),
        [&] (const ComponentDesc&, C2CompPtr, C2CompIntfPtr comp_intf) {

        // check query through stack placeholders and the same with heap allocated
        std::vector<std::unique_ptr<C2Param>> heap_params;
        const C2ParamValues& const_values = GetConstParamValues();
        c2_blocking_t may_block{C2_MAY_BLOCK};
        c2_status_t res = comp_intf->query_vb(const_values.GetStackPointers(),
            const_values.GetIndices(), may_block, &heap_params);
        EXPECT_EQ(res, C2_OK);

        const_values.CheckStackValues();
        const_values.Check(heap_params, false);
    }); // CallComponentTest
}

// This test runs Decode on streams by different decoders
// on different decoding conditions
// (like how streams are split into chunks supplied to decoder).
// Name Check is chosen to produce readable test name like:
// MfxComponents/DecoderDecode.Check/BrokenHeader_C2_h264vd
TEST_P(DecoderDecode, Check)
{
    DecodingConditions conditions = std::get<DecodingConditions>(GetParam());

    CallComponentTest<ComponentDesc>(std::get<ComponentDesc>(GetParam()),
        [&] (const ComponentDesc& desc, C2CompPtr comp, C2CompIntfPtr comp_intf) {

        const int TESTS_COUNT = conditions.repeat_count;

        std::map<bool, std::string> memory_names = {
            { false, "(system memory)" },
            { true, "(video memory)" },
        };

        for (const std::vector<const StreamDescription*>& streams : desc.streams) {

            if (conditions.skip && conditions.skip(streams, desc)) continue;

            SCOPED_TRACE("Stream: " + std::to_string(&streams - desc.streams.data()));

            for (int i = 0; i < TESTS_COUNT; ++i) {

                for (bool use_graphics_memory : { false, true }) {

                    CRC32Generator crc_generator;

                    GTestBinaryWriter writer(std::ostringstream()
                        << comp_intf->getName() << "-" << GetStreamsCombinedName(streams) << "-" << i << ".nv12");

                    DecoderConsumer::OnFrame on_frame = [&] (uint32_t width, uint32_t height,
                        const uint8_t* data, size_t length) {

                        writer.Write(data, length);
                        crc_generator.AddData(width, height, data, length);
                    };

                    std::shared_ptr<DecoderConsumer> validator =
                        std::make_shared<DecoderConsumer>(on_frame);
                    std::list<StreamChunk> stream_chunks = ReadChunks(streams);
                    if (conditions.chunks_mutator) {
                        conditions.chunks_mutator(streams, &stream_chunks);
                    }

                    Decode(use_graphics_memory, comp, validator, streams, stream_chunks);

                    std::list<uint32_t> actual_crc = crc_generator.GetCrc32();
                    std::list<uint32_t> expected_crc;
                    std::transform(streams.begin(), streams.end(), std::back_inserter(expected_crc),
                        [] (const StreamDescription* stream) { return stream->crc32_nv12; } );

                    EXPECT_EQ(actual_crc, expected_crc) << "Pass " << i << " not equal to reference CRC32"
                        << memory_names[use_graphics_memory];
                }
            }
        }
    } );
}

INSTANTIATE_TEST_CASE_P(MfxComponents, CreateDecoder,
    ::testing::ValuesIn(g_components_desc),
    ::testing::PrintToStringParamName());

INSTANTIATE_TEST_CASE_P(MfxInvalidComponents, CreateDecoder,
    ::testing::ValuesIn(g_invalid_components_desc),
    ::testing::PrintToStringParamName());

INSTANTIATE_TEST_CASE_P(MfxComponents, Decoder,
    ::testing::ValuesIn(g_components_desc),
    ::testing::PrintToStringParamName());

INSTANTIATE_TEST_CASE_P(MfxComponents, DecoderDecode,
    ::testing::Combine(
        ::testing::ValuesIn(g_decoding_conditions),
        ::testing::ValuesIn(g_components_desc)),
    ::testing::PrintToStringParamName());
