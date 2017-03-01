// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include "device.h"
#include "image.h"
#include <array>
#include <set>
#include <unordered_set>
#include "algo.h"
using namespace rsimpl;


rs2_frame* endpoint::alloc_frame(size_t size, frame_additional_data additional_data) const
{
    auto frame = _archive->alloc_frame(size, additional_data, true);
    return _archive->track_frame(frame);
}

void endpoint::invoke_callback(frame_holder frame) const
{
    if (frame)
    {
        auto callback = _archive->begin_callback();
        try
        {
            frame->log_callback_start(_ts->get_time());
            if (_callback)
            {
                rs2_frame* ref = nullptr;
                std::swap(frame.frame, ref);
                _callback->on_frame(ref);
                //ref->log_callback_end();          // This reference point would allow to measure
                                                    // user callback lifetime rather than frame data lifetime
            }
        }
        catch(...)
        {
            LOG_ERROR("Exception was thrown during user callback!");
        }
    }
}

void endpoint::flush() const
{
    if (_archive.get())
        _archive->flush();
}

bool endpoint::try_get_pf(const uvc::stream_profile& p, native_pixel_format& result) const
{
    auto it = std::find_if(begin(_pixel_formats), end(_pixel_formats),
        [&p](const native_pixel_format& pf)
    {
        return pf.fourcc == p.format;
    });
    if (it != end(_pixel_formats))
    {
        result = *it;
        return true;
    }
    return false;
}

std::vector<request_mapping> endpoint::resolve_requests(std::vector<stream_profile> requests)
{
    // per requested profile, find all 4ccs that support that request.
    std::map<rs2_stream, std::set<uint32_t>> legal_fourccs;
    auto profiles = get_stream_profiles();
    for (auto&& request : requests) {
         for (auto&& mode : profiles) {
            if (mode.fps == request.fps && mode.height == request.height && mode.width == request.width)
                legal_fourccs[request.stream].insert(mode.format);
        }
    }

    //if you want more efficient data structure use std::unordered_set
    //with well-defined hash function
    std::set <request_mapping> results;

    while (!requests.empty() && !_pixel_formats.empty())
    {
        auto max = 0;
        size_t best_size = 0;
        auto best_pf = &_pixel_formats.front();
        auto best_unpacker = &_pixel_formats.front().unpackers.front();
        for (auto&& pf : _pixel_formats)
        {
            // Speeds up algorithm by skipping obviously useless 4ccs
            // if (std::none_of(begin(legal_fourccs), end(legal_fourccs), [&](const uint32_t fourcc) {return fourcc == pf.fourcc; })) continue;
            for (auto&& unpacker : pf.unpackers)
            {
                auto count = static_cast<int>(std::count_if(begin(requests), end(requests),
                    [&pf, &legal_fourccs, &unpacker](stream_profile& r)
                {
                    // only count if the 4cc can be unpacked into the relevant stream/format
                    // and also, the pixel format can be streamed in the requested dimensions/fps.
                    return unpacker.satisfies(r) && legal_fourccs[r.stream].count(pf.fourcc);
                }));

                // Here we check if the current pixel format / unpacker combination is better than the current best.
                // We judge on two criteria. A: how many of the requested streams can we supply? B: how many total streams do we open?
                // Optimally, we want to find a combination that supplies all the requested streams, and no additional streams.
                if (
                    count > max                                 // If the current combination supplies more streams, it is better.
                    || (count == max                            // Alternatively, if it supplies the same number of streams,
                        && unpacker.outputs.size() < best_size) // but this combination opens fewer total streams, it is also better
                    )
                {
                    max = count;
                    best_size = unpacker.outputs.size();
                    best_pf = &pf;
                    best_unpacker = &unpacker;
                }
            }
        }

        if (max == 0) break;

        requests.erase(std::remove_if(begin(requests), end(requests),
            [best_unpacker, best_pf, &results, &legal_fourccs, this](stream_profile& r)
        {
            if (best_unpacker->satisfies(r) && legal_fourccs[r.stream].count(best_pf->fourcc))
            {
                request_mapping mapping;
                mapping.profile = { r.width, r.height, r.fps, best_pf->fourcc };
                mapping.unpacker = best_unpacker;
                mapping.pf = best_pf;

                results.insert(mapping);
                return true;
            }
            return false;
        }), end(requests));
    }

    if (requests.empty()) return{ begin(results), end(results) };

    throw invalid_value_exception("Subdevice unable to satisfy stream requests!");
}

uvc_endpoint::~uvc_endpoint()
{
    try
    {
        if (_is_streaming)
            stop_streaming();

        if (_is_opened)
            close();
    }
    catch(...)
    {
        LOG_ERROR("An error has occurred while stop_streaming()!");
    }
}

std::vector<uvc::stream_profile> uvc_endpoint::init_stream_profiles()
{
    power on(shared_from_this());
    return _device->get_profiles();
}

std::vector<stream_profile> uvc_endpoint::get_principal_requests()
{
    std::unordered_set<stream_profile> results;
    std::set<uint32_t> unutilized_formats;
    std::set<uint32_t> supported_formats;

    auto profiles = get_stream_profiles();
    for (auto&& p : profiles)
    {
        supported_formats.insert(p.format);
        native_pixel_format pf{};
        if (try_get_pf(p, pf))
        {
            for (auto&& unpacker : pf.unpackers)
            {
                for (auto&& output : unpacker.outputs)
                {
                    results.insert({ output.first, p.width, p.height, p.fps, output.second });
                }
            }
        }
        else
        {
            unutilized_formats.insert(p.format);
        }
    }

    for (auto& elem : unutilized_formats)
    {
        uint32_t device_fourcc = reinterpret_cast<const big_endian<uint32_t>&>(elem);
        char fourcc[sizeof(device_fourcc) + 1];
        memcpy(fourcc, &device_fourcc, sizeof(device_fourcc));
        fourcc[sizeof(device_fourcc)] = 0;
        LOG_WARNING("Unutilized format " << fourcc);
    }

    if (!unutilized_formats.empty())
    {
        std::stringstream ss;
        for (auto& elem : supported_formats)
        {
            uint32_t device_fourcc = reinterpret_cast<const big_endian<uint32_t>&>(elem);
            char fourcc[sizeof(device_fourcc) + 1];
            memcpy(fourcc, &device_fourcc, sizeof(device_fourcc));
            fourcc[sizeof(device_fourcc)] = 0;
            ss << fourcc << std::endl;
        }
        LOG_WARNING("\nDevice supported formats:\n" << ss.str());
    }

    // Sort the results to make sure that the user will receive predictable deterministic output from the API
    std::vector<stream_profile> res{ begin(results), end(results) };
    std::sort(res.begin(), res.end(), [](const stream_profile& a, const stream_profile& b)
    {
        auto at = std::make_tuple(a.stream, a.width, a.height, a.fps, a.format);
        auto bt = std::make_tuple(b.stream, b.width, b.height, b.fps, b.format);

        return at > bt;
    });

    return res;
}

void uvc_endpoint::open(const std::vector<stream_profile>& requests)
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("open(...) failed. UVC device is streaming!");
    else if (_is_opened)
        throw wrong_api_call_sequence_exception("open(...) failed. UVC device is already opened!");

    auto on = std::unique_ptr<power>(new power(shared_from_this()));
    _archive = std::make_shared<frame_archive>(&_max_publish_list_size,_ts);
    auto mapping = resolve_requests(requests);

    auto timestamp_reader = _timestamp_reader.get();

    std::vector<request_mapping> commited;
    for (auto& mode : mapping)
    {
        try
        {
            _device->probe_and_commit(mode.profile,
            [this, mode, timestamp_reader, requests](uvc::stream_profile p, uvc::frame_object f) mutable
            {

            auto system_time = _ts->get_time();

            if (!this->is_streaming())
            {
                LOG_WARNING("Frame received with streaming inactive," << rsimpl::get_string(mode.unpacker->outputs.front().first)
                        << ", Arrived," << std::fixed << system_time);
                return;
            }

            // Ignore any frames which appear corrupted or invalid
            // Determine the timestamp for this frame
            auto timestamp = timestamp_reader->get_frame_timestamp(mode, f);
            auto timestamp_domain = timestamp_reader->get_frame_timestamp_domain(mode, f);

            auto frame_counter = timestamp_reader->get_frame_counter(mode, f);

            auto width = mode.profile.width;
            auto height = mode.profile.height;
            auto fps = mode.profile.fps;

            std::vector<byte *> dest;
            std::vector<frame_holder> refs;

            //frame_drops_status->was_initialized = true;

            // Not updating prev_frame_counter when first frame arrival
            /*if (frame_drops_status->was_initialized)
            {
                frames_drops_counter.fetch_add(int(frame_counter - frame_drops_status->prev_frame_counter - 1));
                frame_drops_status->prev_frame_counter = frame_counter;
            }*/
            auto&& unpacker = *mode.unpacker;
            for (auto&& output : unpacker.outputs)
            {
                LOG_DEBUG("FrameAccepted," << rsimpl::get_string(output.first) << "," << frame_counter
                    << ",Arrived," << std::fixed << system_time
                    << ",TS," << std::fixed << timestamp << ",TS_Domain," << rs2_timestamp_domain_to_string(timestamp_domain));

                auto bpp = get_image_bpp(output.second);
                frame_additional_data additional_data(timestamp,
                    frame_counter,
                    system_time,
                    width,
                    height,
                    fps,
                    width,
                    bpp,
                    output.second,
                    output.first);


                frame_holder frame = this->alloc_frame(width * height * bpp / 8, additional_data);
                if (frame.frame)
                {
                    frame->get()->set_timestamp_domain(timestamp_domain);
                    dest.push_back(const_cast<byte*>(frame->get()->get_frame_data()));
                    refs.push_back(std::move(frame));
                }
                else
                {
                    LOG_INFO("Dropped frame. alloc_frame(...) returned nullptr");
                    return;
                }

                // Obtain buffers for unpacking the frame
                //dest.push_back(archive->alloc_frame(output.first, additional_data, requires_processing));
            }

            // Unpack the frame
            //if (requires_processing)
            if (dest.size() > 0)
            {
                unpacker.unpack(dest.data(), reinterpret_cast<const byte *>(f.pixels), width * height);
            }

            // If any frame callbacks were specified, dispatch them now
            for (auto&& pref : refs)
            {
                // all the streams the unpacker generates are handled here.
                // If it matches one of the streams the user requested, send it to the user.
                if (std::any_of(begin(requests), end(requests), [&pref](stream_profile request) { return request.stream == pref->get()->get_stream_type(); }))
                {
                    if (_on_before_frame_callback)
                    {
                        auto callback = _archive->begin_callback();
                        auto stream_type = pref->get()->get_stream_type();
                        _on_before_frame_callback(stream_type, *pref, std::move(callback));
                    }

                    this->invoke_callback(std::move(pref));
                }
             }
            });
        }
        catch(...)
        {
            for (auto&& commited_mode : commited)
            {
                _device->close(mode.profile);
            }
            throw;
        }
        commited.push_back(mode);
    }

    for (auto& mode : mapping)
    {
        _configuration.push_back(mode.profile);
    }
    _power = move(on);
    _is_opened = true;

    try {
        _device->stream_on();
    }
    catch (...)
    {
        for (auto& profile : _configuration)
        {
            try {
                _device->close(profile);
            }
            catch (...) {}
        }
        reset_streaming();
        _is_opened = false;
        throw;
    }
}

void uvc_endpoint::close()
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("close() failed. UVC device is streaming!");
    else if (!_is_opened)
        throw wrong_api_call_sequence_exception("close() failed. UVC device was not opened!");

    for (auto& profile : _configuration)
    {
        _device->close(profile);
    }
    reset_streaming();
    _power.reset();
    _is_opened = false;
}

void uvc_endpoint::register_xu(uvc::extension_unit xu)
{
    _xus.push_back(std::move(xu));
}

void uvc_endpoint::start_streaming(frame_callback_ptr callback)
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("start_streaming(...) failed. UVC device is already streaming!");
    else if(!_is_opened)
        throw wrong_api_call_sequence_exception("start_streaming(...) failed. UVC device was not opened!");

    _callback = std::move(callback);
    _is_streaming = true;
    _device->start_callbacks();
}

void uvc_endpoint::stop_streaming()
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (!_is_streaming)
        throw wrong_api_call_sequence_exception("stop_streaming() failed. UVC device is not streaming!");

    _is_streaming = false;
    _device->stop_callbacks();
}

void uvc_endpoint::reset_streaming()
{
    flush();
    _configuration.clear();
    _callback.reset();
    _archive.reset();
    _timestamp_reader->reset();
}

void uvc_endpoint::acquire_power()
{
    if (_user_count.fetch_add(1) == 0)
    {
        _device->set_power_state(uvc::D0);
        for (auto& xu : _xus) _device->init_xu(xu);
    }
}

void uvc_endpoint::release_power()
{
    if (_user_count.fetch_add(-1) == 1) _device->set_power_state(uvc::D3);
}

option& endpoint::get_option(rs2_option id)
{
    auto it = _options.find(id);
    if (it == _options.end())
    {
        throw invalid_value_exception(to_string()
            << "Device does not support option "
            << rs2_option_to_string(id) << "!");
    }
    return *it->second;
}

const option& endpoint::get_option(rs2_option id) const
{
    auto it = _options.find(id);
    if (it == _options.end())
    {
        throw invalid_value_exception(to_string()
            << "Device does not support option "
            << rs2_option_to_string(id) << "!");
    }
    return *it->second;
}

bool endpoint::supports_option(rs2_option id) const
{
    auto it = _options.find(id);
    if (it == _options.end()) return false;
    return it->second->is_enabled();
}

bool endpoint::supports_info(rs2_camera_info info) const
{
    auto it = _camera_info.find(info);
    return it != _camera_info.end();
}

void endpoint::register_info(rs2_camera_info info, const std::string& val)
{
    if (supports_info(info) && (get_info(info) != val)) // Append existing infos
    {
        _camera_info[info] += "\n" + std::move(val);
    }
    else
    {
        _camera_info[info] = std::move(val);
    }
}

const std::string& endpoint::get_info(rs2_camera_info info) const
{
    auto it = _camera_info.find(info);
    if (it == _camera_info.end())
        throw invalid_value_exception("Selected camera info is not supported for this camera!");

    return it->second;
}

void endpoint::register_option(rs2_option id, std::shared_ptr<option> option)
{
    _options[id] = std::move(option);
}

void uvc_endpoint::register_pu(rs2_option id)
{
    register_option(id, std::make_shared<uvc_pu_option>(*this, id));
}

hid_endpoint::~hid_endpoint()
{
    try
    {
        if (_is_streaming)
            stop_streaming();

        if (_is_opened)
            close();
    }
    catch(...)
    {
        LOG_ERROR("An error has occurred while stop_streaming()!");
    }
}

std::vector<uvc::stream_profile> hid_endpoint::init_stream_profiles()
{
    std::unordered_set<uvc::stream_profile> results;
    for (auto& elem : get_device_profiles())
    {
        results.insert({elem.width, elem.height, elem.fps, stream_to_fourcc(elem.stream)});
    }
    return std::vector<uvc::stream_profile>(results.begin(), results.end());
}

std::vector<stream_profile> hid_endpoint::get_sensor_profiles(std::string sensor_name) const
{
    std::vector<stream_profile> profiles{};
    for (auto& elem : _sensor_name_and_hid_profiles)
    {
        if (!elem.first.compare(sensor_name))
        {
            profiles.push_back(elem.second);
        }
    }

    return profiles;
}

std::vector<stream_profile> hid_endpoint::get_principal_requests()
{
    return get_device_profiles();
}

void hid_endpoint::open(const std::vector<stream_profile>& requests)
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("open(...) failed. Hid device is streaming!");
    else if (_is_opened)
        throw wrong_api_call_sequence_exception("Hid device is already opened!");

    auto mapping = resolve_requests(requests);
    _hid_device->open();
    for (auto& request : requests)
    {
        auto sensor_iio = rs2_stream_to_sensor_iio(request.stream);
        for (auto& map : mapping)
        {
            auto it = std::find_if(begin(map.unpacker->outputs), end(map.unpacker->outputs),
                                   [&](const std::pair<rs2_stream, rs2_format>& pair)
            {
                return pair.first == request.stream;
            });

            if (it != end(map.unpacker->outputs))
            {
                _configured_profiles.insert(std::make_pair(sensor_iio,
                                                           stream_profile{request.stream,
                                                                          request.width,
                                                                          request.height,
                                                                          fps_to_sampling_frequency(request.stream, request.fps),
                                                                          request.format}));
                _iio_mapping.insert(std::make_pair(sensor_iio, map));
            }
        }
    }
    _is_opened = true;
}

void hid_endpoint::close()
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("close() failed. Hid device is streaming!");
    else if (!_is_opened)
        throw wrong_api_call_sequence_exception("close() failed. Hid device was not opened!");

    _hid_device->close();
    _configured_profiles.clear();
    _iio_mapping.clear();
    _is_opened = false;
}

void hid_endpoint::start_streaming(frame_callback_ptr callback)
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (_is_streaming)
        throw wrong_api_call_sequence_exception("start_streaming(...) failed. Hid device is already streaming!");
    else if(!_is_opened)
        throw wrong_api_call_sequence_exception("start_streaming(...) failed. Hid device was not opened!");

    _archive = std::make_shared<frame_archive>(&_max_publish_list_size, _ts);
    _callback = std::move(callback);
    std::vector<uvc::iio_profile> configured_iio_profiles;
    for (auto& elem : _configured_profiles)
    {
        configured_iio_profiles.push_back(uvc::iio_profile{elem.first, elem.second.fps});
    }


    _hid_device->start_capture(configured_iio_profiles, [this](const uvc::sensor_data& sensor_data)
    {
        auto system_time = _ts->get_time();

        if (!this->is_streaming())
        {
            LOG_INFO("HID Frame received when Streaming is not active,"
                        << get_string(_configured_profiles[sensor_data.sensor.iio].stream)
                        << ",Arrived," << std::fixed << system_time);
            return;
        }

        auto mode = _iio_mapping[sensor_data.sensor.iio];
        auto data_size = sensor_data.fo.frame_size;
        mode.profile.width = (uint32_t)data_size;
        mode.profile.height = 1;

        // Determine the timestamp for this HID frame
        auto timestamp = _timestamp_reader->get_frame_timestamp(mode, sensor_data.fo);
        auto frame_counter = _timestamp_reader->get_frame_counter(mode, sensor_data.fo);

        frame_additional_data additional_data;
        additional_data.format = _configured_profiles[sensor_data.sensor.iio].format;
        additional_data.stream_type = _configured_profiles[sensor_data.sensor.iio].stream;
        additional_data.width = mode.profile.width;
        additional_data.height = mode.profile.height;
        additional_data.timestamp = timestamp;
        additional_data.frame_number = frame_counter;
        additional_data.timestamp_domain = _timestamp_reader->get_frame_timestamp_domain(mode, sensor_data.fo);

        LOG_DEBUG("FrameAccepted," << get_string(additional_data.stream_type) << "," << frame_counter
                  << ",Arrived," << std::fixed << system_time
                  << ",TS," << timestamp
                  << ",TS_Domain," << rs2_timestamp_domain_to_string(additional_data.timestamp_domain));

        auto frame = this->alloc_frame(data_size, additional_data);
        if (!frame)
        {
            LOG_INFO("Dropped frame. alloc_frame(...) returned nullptr");
            return;
        }

        std::vector<byte*> dest{const_cast<byte*>(frame->get()->data.data())};
        mode.unpacker->unpack(dest.data(),(const byte*)sensor_data.fo.pixels, (int)data_size);
        this->invoke_callback(frame);
    });

    _is_streaming = true;
}

void hid_endpoint::stop_streaming()
{
    std::lock_guard<std::mutex> lock(_configure_lock);
    if (!_is_streaming)
        throw wrong_api_call_sequence_exception("stop_streaming() failed. Hid device is not streaming!");


    _hid_device->stop_capture();
    _is_streaming = false;
    flush();
    _callback.reset();
    _archive.reset();
    _timestamp_reader->reset();
}

std::vector<stream_profile> hid_endpoint::get_device_profiles()
{
    std::vector<stream_profile> stream_requests;
    for (auto& sensor : _hid_sensors)
    {
        auto profiles = get_sensor_profiles(sensor.name);
        stream_requests.insert(stream_requests.end(), profiles.begin() ,profiles.end());
    }

    return stream_requests;
}

uint32_t hid_endpoint::rs2_stream_to_sensor_iio(rs2_stream stream) const
{
    for (auto& elem : _sensor_name_and_hid_profiles)
    {
        if (stream == elem.second.stream)
            return get_iio_by_name(elem.first);
    }
    throw invalid_value_exception("rs2_stream not found!");
}

uint32_t hid_endpoint::get_iio_by_name(const std::string& name) const
{
    for (auto& elem : _hid_sensors)
    {
        if (!elem.name.compare(name))
            return elem.iio;
    }
    throw invalid_value_exception("sensor_name not found!");
}

uint32_t hid_endpoint::stream_to_fourcc(rs2_stream stream) const
{
    uint32_t fourcc;
    try{
        fourcc = stream_and_fourcc.at(stream);
    }
    catch(std::out_of_range)
    {
        throw invalid_value_exception(to_string() << "fourcc of stream " << rs2_stream_to_string(stream) << " not found!");
    }

    return fourcc;
}

uint32_t hid_endpoint::fps_to_sampling_frequency(rs2_stream stream, uint32_t fps) const
{
    uint32_t sampling_frequency = 0;
    try{
        sampling_frequency = _fps_and_sampling_frequency_per_rs2_stream.at(stream).at(fps);
    }
    catch(std::out_of_range)
    {
        throw invalid_value_exception(to_string() << "sampling_frequency of fps " << fps << " not found!");
    }

    return sampling_frequency;
}
