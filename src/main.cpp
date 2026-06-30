#include "Spinnaker.h"
#include "ImageUtility.h"
#include "SpinGenApi/SpinnakerGenApi.h"

#include <arpa/inet.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

namespace
{
std::atomic<bool> g_running{true};

void handleSignal(int)
{
    g_running = false;
}

uint32_t parseUint32(const std::string& value, const char* name, uint32_t minValue, uint32_t maxValue)
{
    size_t parsed = 0;
    unsigned long number = std::stoul(value, &parsed);
    if (parsed != value.size() || number < minValue || number > maxValue)
    {
        std::ostringstream message;
        message << name << " must be between " << minValue << " and " << maxValue;
        throw std::runtime_error(message.str());
    }
    return static_cast<uint32_t>(number);
}

std::string jsonEscape(const std::string& input)
{
    std::ostringstream out;
    for (char c : input)
    {
        switch (c)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c));
            }
            else
            {
                out << c;
            }
        }
    }
    return out.str();
}

std::string urlDecode(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '%' && i + 2 < input.size())
        {
            char hex[3] = {input[i + 1], input[i + 2], '\0'};
            char* end = nullptr;
            long value = std::strtol(hex, &end, 16);
            if (end == hex + 2)
            {
                out.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i] == '+' ? ' ' : input[i]);
    }
    return out;
}

std::string queryValue(const std::string& query, const std::string& key)
{
    size_t start = 0;
    while (start <= query.size())
    {
        size_t end = query.find('&', start);
        std::string pair = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t equals = pair.find('=');
        std::string name = urlDecode(pair.substr(0, equals));
        if (name == key)
        {
            return equals == std::string::npos ? std::string() : urlDecode(pair.substr(equals + 1));
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return {};
}

std::string httpDate()
{
    char buffer[128];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buffer;
}

bool sendAll(int fd, const uint8_t* data, size_t length)
{
    while (length > 0)
    {
        ssize_t written = ::send(fd, data, length, MSG_NOSIGNAL);
        if (written <= 0)
        {
            return false;
        }
        data += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

bool sendAll(int fd, const std::string& data)
{
    return sendAll(fd, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool setEnumIfAvailable(INodeMap& nodeMap, const char* nodeName, const char* entryName)
{
    CEnumerationPtr node = nodeMap.GetNode(nodeName);
    if (!IsAvailable(node) || !IsReadable(node) || !IsWritable(node))
    {
        return false;
    }

    CEnumEntryPtr entry = node->GetEntryByName(entryName);
    if (!IsAvailable(entry) || !IsReadable(entry))
    {
        return false;
    }

    node->SetIntValue(entry->GetValue());
    return true;
}

std::string readStringNode(INodeMap& nodeMap, const char* nodeName)
{
    CStringPtr node = nodeMap.GetNode(nodeName);
    if (IsAvailable(node) && IsReadable(node))
    {
        return std::string(node->GetValue().c_str());
    }
    return {};
}

std::string readEnumNode(INodeMap& nodeMap, const char* nodeName)
{
    CEnumerationPtr node = nodeMap.GetNode(nodeName);
    if (IsAvailable(node) && IsReadable(node))
    {
        CEnumEntryPtr entry = node->GetCurrentEntry();
        if (IsAvailable(entry) && IsReadable(entry))
        {
            return std::string(entry->GetSymbolic().c_str());
        }
    }
    return {};
}

std::vector<uint8_t> readFileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        throw std::runtime_error("failed to open encoded JPEG");
    }

    const std::streamoff size = in.tellg();
    if (size <= 0)
    {
        throw std::runtime_error("encoded JPEG is empty");
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        throw std::runtime_error("failed to read encoded JPEG");
    }
    return bytes;
}

std::vector<uint8_t> makeJpegFromImage(const ImagePtr& image, uint32_t quality)
{
    static const std::string path = "/tmp/cam_website_frame_" + std::to_string(::getpid()) + ".jpg";

    JPEGOption option;
    option.quality = quality;
    option.progressive = false;
    image->Save(path.c_str(), option);

    std::vector<uint8_t> bytes = readFileBytes(path);
    std::remove(path.c_str());
    return bytes;
}

struct AppConfig
{
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 8080;
    std::string serial;
    uint32_t jpegQuality = 75;
    uint32_t streamMaxWidth = 1280;
    uint32_t streamFps = 10;
};

struct CameraInfo
{
    std::string serial;
    std::string model;
    std::string accessStatus;
};

class CameraStreamer
{
  public:
    explicit CameraStreamer(std::string serial, uint32_t jpegQuality, uint32_t streamMaxWidth, uint32_t streamFps)
        : selectedSerial_(std::move(serial)),
          jpegQuality_(jpegQuality),
          streamMaxWidth_(streamMaxWidth),
          streamFps_(streamFps)
    {
    }

    void start()
    {
        worker_ = std::thread(&CameraStreamer::captureLoop, this);
        discoveryWorker_ = std::thread(&CameraStreamer::discoveryLoop, this);
    }

    void stop()
    {
        running_ = false;
        if (discoveryWorker_.joinable())
        {
            discoveryWorker_.join();
        }
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    void selectCamera(const std::string& serial)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial == selectedSerial_)
        {
            return;
        }

        selectedSerial_ = serial;
        switchRequested_ = true;
        connected_ = false;
        latestJpeg_.clear();
        width_ = 0;
        height_ = 0;
        frameId_ = 0;
        lastError_ = serial.empty() ? "Switching to first available camera" : "Switching to camera " + serial;
    }

    bool latestFrame(std::vector<uint8_t>& frame, uint64_t& frameId, size_t& width, size_t& height) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latestJpeg_.empty())
        {
            return false;
        }
        frame = latestJpeg_;
        frameId = frameId_;
        width = width_;
        height = height_;
        return true;
    }

    std::string statusJson() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "{";
        out << "\"running\":" << (running_ ? "true" : "false") << ",";
        out << "\"connected\":" << (connected_ ? "true" : "false") << ",";
        out << "\"frame_id\":" << frameId_ << ",";
        out << "\"width\":" << width_ << ",";
        out << "\"height\":" << height_ << ",";
        out << "\"jpeg_quality\":" << jpegQuality_ << ",";
        out << "\"stream_max_width\":" << streamMaxWidth_ << ",";
        out << "\"stream_fps\":" << streamFps_ << ",";
        out << "\"camera_count\":" << cameraCount_ << ",";
        out << "\"selected_serial\":\"" << jsonEscape(selectedSerial_) << "\",";
        out << "\"serial\":\"" << jsonEscape(serial_) << "\",";
        out << "\"model\":\"" << jsonEscape(model_) << "\",";
        out << "\"access_status\":\"" << jsonEscape(accessStatus_) << "\",";
        out << "\"last_error\":\"" << jsonEscape(lastError_) << "\"";
        out << "}";
        return out.str();
    }

    std::string camerasJson() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "{";
        out << "\"selected_serial\":\"" << jsonEscape(selectedSerial_) << "\",";
        out << "\"active_serial\":\"" << jsonEscape(serial_) << "\",";
        out << "\"cameras\":[";
        for (size_t i = 0; i < cameras_.size(); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            const CameraInfo& cam = cameras_[i];
            out << "{";
            out << "\"serial\":\"" << jsonEscape(cam.serial) << "\",";
            out << "\"model\":\"" << jsonEscape(cam.model) << "\",";
            out << "\"access_status\":\"" << jsonEscape(cam.accessStatus) << "\",";
            out << "\"selected\":" << (cam.serial == selectedSerial_ ? "true" : "false") << ",";
            out << "\"active\":" << (cam.serial == serial_ && connected_ ? "true" : "false");
            out << "}";
        }
        out << "]}";
        return out.str();
    }

  private:
    void updateCameraList(const std::vector<CameraInfo>& cameras)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cameras_ = cameras;
        cameraCount_ = cameras_.size();
    }

    void setStatus(bool connected, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = connected;
        lastError_ = error;
        if (!connected)
        {
            width_ = 0;
            height_ = 0;
            latestJpeg_.clear();
        }
    }

    std::string selectedSerial() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return selectedSerial_;
    }

    bool shouldSwitch(const std::string& activeSerial) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return switchRequested_ || (!selectedSerial_.empty() && selectedSerial_ != activeSerial);
    }

    void markCameraOpened(const std::string& serial, const std::string& model, const std::string& accessStatus)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        serial_ = serial;
        model_ = model;
        accessStatus_ = accessStatus;

        if (selectedSerial_.empty())
        {
            selectedSerial_ = serial;
        }

        if (selectedSerial_ == serial)
        {
            switchRequested_ = false;
            lastError_.clear();
        }
    }

    std::vector<CameraInfo> readCameraList(CameraList& camList)
    {
        std::vector<CameraInfo> cameras;
        cameras.reserve(camList.GetSize());
        for (unsigned int i = 0; i < camList.GetSize(); ++i)
        {
            CameraPtr cam = camList.GetByIndex(i);
            INodeMap& tl = cam->GetTLDeviceNodeMap();
            cameras.push_back(
                {readStringNode(tl, "DeviceSerialNumber"), readStringNode(tl, "DeviceModelName"),
                 readEnumNode(tl, "DeviceAccessStatus")});
        }
        return cameras;
    }

    CameraPtr chooseCamera(CameraList& camList, const std::string& requestedSerial)
    {
        const size_t cameraCount = static_cast<size_t>(camList.GetSize());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cameraCount_ = cameraCount;
        }

        if (cameraCount == 0)
        {
            return nullptr;
        }

        if (requestedSerial.empty())
        {
            return camList.GetByIndex(0);
        }

        for (unsigned int i = 0; i < camList.GetSize(); ++i)
        {
            CameraPtr cam = camList.GetByIndex(i);
            INodeMap& tl = cam->GetTLDeviceNodeMap();
            if (readStringNode(tl, "DeviceSerialNumber") == requestedSerial)
            {
                return cam;
            }
        }

        return nullptr;
    }

    void captureOnce()
    {
        std::unique_lock<std::mutex> sdkLock(sdkMutex_);
        SystemPtr system = System::GetInstance();
        CameraList camList = system->GetCameras();
        updateCameraList(readCameraList(camList));
        const std::string requestedSerial = selectedSerial();

        CameraPtr cam = chooseCamera(camList, requestedSerial);
        if (cam == nullptr)
        {
            std::ostringstream error;
            error << "No matching camera found";
            if (!requestedSerial.empty())
            {
                error << " for serial " << requestedSerial;
            }
            setStatus(false, error.str());
            camList.Clear();
            system->ReleaseInstance();
            sdkLock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            return;
        }

        INodeMap& tlDevice = cam->GetTLDeviceNodeMap();
        std::string cameraSerial = readStringNode(tlDevice, "DeviceSerialNumber");
        std::string cameraModel = readStringNode(tlDevice, "DeviceModelName");
        std::string cameraAccessStatus = readEnumNode(tlDevice, "DeviceAccessStatus");
        markCameraOpened(cameraSerial, cameraModel, cameraAccessStatus);

        if (shouldSwitch(cameraSerial))
        {
            cam = nullptr;
            camList.Clear();
            system->ReleaseInstance();
            sdkLock.unlock();
            return;
        }

        bool acquiring = false;

        try
        {
            cam->Init();

            INodeMap& nodeMap = cam->GetNodeMap();
            setEnumIfAvailable(nodeMap, "AcquisitionMode", "Continuous");
            setEnumIfAvailable(cam->GetTLStreamNodeMap(), "StreamBufferHandlingMode", "NewestOnly");

            ImageProcessor processor;
            processor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_HQ_LINEAR);
            auto lastEncoded = std::chrono::steady_clock::time_point::min();
            const auto encodeInterval = std::chrono::milliseconds(1000 / std::max<uint32_t>(1, streamFps_));

            cam->BeginAcquisition();
            acquiring = true;
            setStatus(true, "");
            sdkLock.unlock();

            while (running_ && g_running)
            {
                if (shouldSwitch(cameraSerial))
                {
                    break;
                }

                ImagePtr image = cam->GetNextImage(1000);
                if (image->IsIncomplete())
                {
                    std::string description = Image::GetImageStatusDescription(image->GetImageStatus());
                    image->Release();
                    setStatus(true, "Incomplete image: " + description);
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (lastEncoded != std::chrono::steady_clock::time_point::min() && now - lastEncoded < encodeInterval)
                {
                    image->Release();
                    continue;
                }

                ImagePtr rgb = processor.Convert(image, PixelFormat_RGB8);
                ImagePtr output = rgb;
                if (streamMaxWidth_ > 0 && rgb->GetWidth() > streamMaxWidth_)
                {
                    const size_t divisor = (rgb->GetWidth() + streamMaxWidth_ - 1) / streamMaxWidth_;
                    const double scale = 1.0 / static_cast<double>(divisor);
                    output = ImageUtility::CreateScaled(rgb, SPINNAKER_IMAGE_SCALING_ALGORITHM_NEAREST_NEIGHBOR, scale);
                }

                std::vector<uint8_t> jpeg = makeJpegFromImage(output, jpegQuality_);
                lastEncoded = now;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latestJpeg_ = std::move(jpeg);
                    frameId_ = image->GetFrameID();
                    width_ = output->GetWidth();
                    height_ = output->GetHeight();
                    connected_ = true;
                    lastError_.clear();
                }

                image->Release();
            }
        }
        catch (const Spinnaker::Exception& e)
        {
            if (sdkLock.owns_lock())
            {
                sdkLock.unlock();
            }
            setStatus(false, e.what());
        }
        catch (const std::exception& e)
        {
            if (sdkLock.owns_lock())
            {
                sdkLock.unlock();
            }
            setStatus(false, e.what());
        }

        sdkLock.lock();
        try
        {
            if (acquiring)
            {
                cam->EndAcquisition();
            }
        }
        catch (const Spinnaker::Exception&)
        {
        }

        try
        {
            cam->DeInit();
        }
        catch (const Spinnaker::Exception&)
        {
        }

        cam = nullptr;
        camList.Clear();
        system->ReleaseInstance();
        sdkLock.unlock();

        if (running_ && g_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    void discoverOnce()
    {
        std::lock_guard<std::mutex> sdkLock(sdkMutex_);
        SystemPtr system = System::GetInstance();
        CameraList camList = system->GetCameras();
        updateCameraList(readCameraList(camList));
        camList.Clear();
        system->ReleaseInstance();
    }

    void discoveryLoop()
    {
        while (running_ && g_running)
        {
            try
            {
                discoverOnce();
            }
            catch (const Spinnaker::Exception&)
            {
            }
            catch (const std::exception&)
            {
            }

            for (int i = 0; i < 15 && running_ && g_running; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void captureLoop()
    {
        while (running_ && g_running)
        {
            try
            {
                captureOnce();
            }
            catch (const Spinnaker::Exception& e)
            {
                setStatus(false, e.what());
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            catch (const std::exception& e)
            {
                setStatus(false, e.what());
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    std::atomic<bool> running_{true};
    std::thread worker_;
    std::thread discoveryWorker_;

    std::mutex sdkMutex_;
    mutable std::mutex mutex_;
    std::vector<uint8_t> latestJpeg_;
    std::vector<CameraInfo> cameras_;
    uint64_t frameId_ = 0;
    size_t width_ = 0;
    size_t height_ = 0;
    size_t cameraCount_ = 0;
    bool connected_ = false;
    bool switchRequested_ = false;
    std::string selectedSerial_;
    std::string serial_;
    std::string model_;
    std::string accessStatus_;
    std::string lastError_ = "Starting";
    uint32_t jpegQuality_ = 75;
    uint32_t streamMaxWidth_ = 1280;
    uint32_t streamFps_ = 10;
};

const char* kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Spinnaker Camera</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #101112;
      --panel: #191b1d;
      --panel-2: #202326;
      --text: #f0f3f5;
      --muted: #9da8b2;
      --line: #30363b;
      --accent: #21b17a;
      --warn: #e2a83b;
      --bad: #e05252;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background: var(--bg);
      color: var(--text);
      font: 14px/1.45 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }
    .app {
      display: grid;
      grid-template-rows: auto 1fr;
      min-height: 100vh;
    }
    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 12px 16px;
      border-bottom: 1px solid var(--line);
      background: var(--panel);
    }
    h1 {
      margin: 0;
      font-size: 18px;
      font-weight: 650;
    }
    .toolbar {
      display: flex;
      align-items: center;
      gap: 10px;
      color: var(--muted);
    }
    .source {
      display: grid;
      gap: 7px;
      margin-bottom: 16px;
    }
    .source label {
      color: var(--muted);
    }
    select {
      width: 100%;
      min-height: 34px;
      border: 1px solid var(--line);
      background: var(--panel-2);
      color: var(--text);
      border-radius: 6px;
      padding: 6px 8px;
    }
    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--warn);
      flex: 0 0 auto;
    }
    .dot.live { background: var(--accent); }
    .dot.offline { background: var(--bad); }
    main {
      display: grid;
      grid-template-columns: minmax(0, 1fr) 320px;
      min-height: 0;
    }
    .viewer {
      position: relative;
      display: grid;
      place-items: center;
      min-height: 0;
      padding: 16px;
      background: #0b0c0d;
    }
    .viewer img {
      max-width: 100%;
      max-height: calc(100vh - 90px);
      object-fit: contain;
      image-rendering: auto;
      border: 1px solid var(--line);
      background: #050607;
    }
    .empty {
      position: absolute;
      inset: 16px;
      display: grid;
      place-items: center;
      color: var(--muted);
      text-align: center;
      pointer-events: none;
    }
    aside {
      border-left: 1px solid var(--line);
      background: var(--panel);
      padding: 16px;
      overflow: auto;
    }
    dl {
      display: grid;
      grid-template-columns: 110px minmax(0, 1fr);
      gap: 9px 12px;
      margin: 0;
    }
    dt {
      color: var(--muted);
    }
    dd {
      margin: 0;
      overflow-wrap: anywhere;
    }
    .error {
      margin-top: 16px;
      padding: 10px 12px;
      border: 1px solid color-mix(in srgb, var(--bad), transparent 40%);
      background: color-mix(in srgb, var(--bad), transparent 88%);
      color: #ffd7d7;
      display: none;
    }
    .error.visible { display: block; }
    button {
      border: 1px solid var(--line);
      background: var(--panel-2);
      color: var(--text);
      padding: 7px 10px;
      border-radius: 6px;
      cursor: pointer;
    }
    button:hover { border-color: #4a535b; }
    @media (max-width: 820px) {
      header { align-items: flex-start; flex-direction: column; }
      main { grid-template-columns: 1fr; }
      aside { border-left: 0; border-top: 1px solid var(--line); }
      .viewer img { max-height: 62vh; }
    }
  </style>
</head>
<body>
  <div class="app">
    <header>
      <h1>Spinnaker Camera</h1>
      <div class="toolbar">
        <span id="dot" class="dot"></span>
        <span id="state">Connecting</span>
        <button id="pause" type="button">Pause</button>
      </div>
    </header>
    <main>
      <section class="viewer">
        <img id="frame" alt="Live camera frame">
        <div id="empty" class="empty">Waiting for camera frames</div>
      </section>
      <aside>
        <div class="source">
          <label for="cameraSelect">Camera</label>
          <select id="cameraSelect"></select>
        </div>
        <dl>
          <dt>Camera</dt><dd id="camera">-</dd>
          <dt>Serial</dt><dd id="serial">-</dd>
          <dt>Access</dt><dd id="access">-</dd>
          <dt>Resolution</dt><dd id="resolution">-</dd>
          <dt>Frame</dt><dd id="frameId">-</dd>
          <dt>Discovered</dt><dd id="count">-</dd>
        </dl>
        <div id="error" class="error"></div>
      </aside>
    </main>
  </div>
  <script>
    const img = document.getElementById('frame');
    const dot = document.getElementById('dot');
    const state = document.getElementById('state');
    const pause = document.getElementById('pause');
    const empty = document.getElementById('empty');
    const error = document.getElementById('error');
    const cameraSelect = document.getElementById('cameraSelect');
    let paused = false;
    let frameDelay = 100;
    let delay = frameDelay;
    let cameraSelectDirty = false;

    function setStatus(s) {
      if (s.stream_fps) {
        frameDelay = Math.max(33, Math.round(1000 / s.stream_fps));
      }
      dot.className = 'dot ' + (s.connected ? 'live' : 'offline');
      state.textContent = s.connected ? 'Live' : 'Offline';
      document.getElementById('camera').textContent = s.model || '-';
      document.getElementById('serial').textContent = s.serial || '-';
      document.getElementById('access').textContent = s.access_status || '-';
      document.getElementById('resolution').textContent = s.width && s.height ? `${s.width} x ${s.height}` : '-';
      document.getElementById('frameId').textContent = s.frame_id || '-';
      document.getElementById('count').textContent = s.camera_count ?? '-';
      empty.style.display = s.connected && s.frame_id ? 'none' : 'grid';
      error.textContent = s.last_error || '';
      error.className = 'error' + (s.last_error ? ' visible' : '');
    }

    function cameraLabel(camera) {
      const model = camera.model || 'Camera';
      const serial = camera.serial || 'unknown';
      const active = camera.active ? ' - active' : '';
      return `${model} (${serial})${active}`;
    }

    function setCameras(data) {
      const currentValue = cameraSelect.value;
      const selected = data.selected_serial || data.active_serial || '';
      const cameras = data.cameras || [];
      cameraSelect.innerHTML = '';

      if (!cameras.length) {
        const option = document.createElement('option');
        option.value = '';
        option.textContent = 'No cameras detected';
        cameraSelect.appendChild(option);
        cameraSelect.disabled = true;
        cameraSelectDirty = false;
        return;
      }

      cameraSelect.disabled = false;
      for (const camera of cameras) {
        const option = document.createElement('option');
        option.value = camera.serial || '';
        option.textContent = cameraLabel(camera);
        cameraSelect.appendChild(option);
      }

      const exists = cameras.some(camera => camera.serial === selected);
      cameraSelect.value = exists ? selected : (cameras[0].serial || '');
      if (cameraSelectDirty && currentValue && cameras.some(camera => camera.serial === currentValue)) {
        cameraSelect.value = currentValue;
      }
      cameraSelectDirty = false;
    }

    async function pollStatus() {
      try {
        const response = await fetch('/status.json', { cache: 'no-store' });
        setStatus(await response.json());
      } catch {
        dot.className = 'dot offline';
        state.textContent = 'Server unavailable';
      } finally {
        setTimeout(pollStatus, 700);
      }
    }

    async function pollCameras() {
      try {
        const response = await fetch('/cameras.json', { cache: 'no-store' });
        setCameras(await response.json());
      } catch {
      } finally {
        setTimeout(pollCameras, 1200);
      }
    }

    function nextFrame() {
      if (paused) return;
      img.src = `/frame.jpg?t=${Date.now()}`;
    }

    img.onload = () => {
      delay = frameDelay;
      empty.style.display = 'none';
      setTimeout(nextFrame, delay);
    };
    img.onerror = () => {
      delay = Math.min(delay + 150, 1500);
      setTimeout(nextFrame, delay);
    };
    pause.onclick = () => {
      paused = !paused;
      pause.textContent = paused ? 'Resume' : 'Pause';
      if (!paused) nextFrame();
    };
    cameraSelect.onchange = async () => {
      cameraSelectDirty = true;
      const serial = cameraSelect.value;
      empty.style.display = 'grid';
      try {
        await fetch(`/select?serial=${encodeURIComponent(serial)}`, { cache: 'no-store' });
      } catch {
      }
    };

    pollStatus();
    pollCameras();
    nextFrame();
  </script>
</body>
</html>
)HTML";

class HttpServer
{
  public:
    HttpServer(AppConfig config, CameraStreamer& streamer) : config_(std::move(config)), streamer_(streamer) {}

    void run()
    {
        int server = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0)
        {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.bindAddress.c_str(), &addr.sin_addr) != 1)
        {
            ::close(server);
            throw std::runtime_error("invalid bind address: " + config_.bindAddress);
        }

        if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            std::string message = "bind failed on " + config_.bindAddress + ":" + std::to_string(config_.port) +
                                  ": " + std::strerror(errno);
            ::close(server);
            throw std::runtime_error(message);
        }

        if (::listen(server, 32) < 0)
        {
            ::close(server);
            throw std::runtime_error("listen failed");
        }

        int flags = ::fcntl(server, F_GETFL, 0);
        ::fcntl(server, F_SETFL, flags | O_NONBLOCK);

        std::cout << "Serving http://" << config_.bindAddress << ":" << config_.port << std::endl;

        while (g_running)
        {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int client = ::accept(server, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (client < 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            std::thread(&HttpServer::handleClient, this, client).detach();
        }

        ::close(server);
    }

  private:
    void sendResponse(int client, int code, const std::string& reason, const std::string& contentType,
                      const std::vector<uint8_t>& body, const std::string& extraHeaders = "")
    {
        std::ostringstream header;
        header << "HTTP/1.1 " << code << " " << reason << "\r\n";
        header << "Date: " << httpDate() << "\r\n";
        header << "Server: cam_website\r\n";
        header << "Content-Type: " << contentType << "\r\n";
        header << "Content-Length: " << body.size() << "\r\n";
        header << "Connection: close\r\n";
        header << extraHeaders;
        header << "\r\n";
        sendAll(client, header.str());
        if (!body.empty())
        {
            sendAll(client, body.data(), body.size());
        }
    }

    void sendText(int client, int code, const std::string& reason, const std::string& contentType,
                  const std::string& body, const std::string& extraHeaders = "")
    {
        std::vector<uint8_t> bytes(body.begin(), body.end());
        sendResponse(client, code, reason, contentType, bytes, extraHeaders);
    }

    void handleClient(int client)
    {
        char buffer[4096];
        ssize_t received = ::recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0)
        {
            ::close(client);
            return;
        }
        buffer[received] = '\0';

        std::istringstream request(buffer);
        std::string method;
        std::string path;
        std::string version;
        request >> method >> path >> version;

        std::string queryString;
        size_t query = path.find('?');
        if (query != std::string::npos)
        {
            queryString = path.substr(query + 1);
            path = path.substr(0, query);
        }

        if (method != "GET" && method != "HEAD")
        {
            sendText(client, 405, "Method Not Allowed", "text/plain; charset=utf-8", "Method not allowed\n");
            ::close(client);
            return;
        }

        if (path == "/" || path == "/index.html")
        {
            sendText(client, 200, "OK", "text/html; charset=utf-8", kIndexHtml,
                     "Cache-Control: no-store\r\n");
        }
        else if (path == "/status.json")
        {
            sendText(client, 200, "OK", "application/json; charset=utf-8", streamer_.statusJson(),
                     "Cache-Control: no-store\r\n");
        }
        else if (path == "/cameras.json")
        {
            sendText(client, 200, "OK", "application/json; charset=utf-8", streamer_.camerasJson(),
                     "Cache-Control: no-store\r\n");
        }
        else if (path == "/select")
        {
            streamer_.selectCamera(queryValue(queryString, "serial"));
            sendText(client, 200, "OK", "application/json; charset=utf-8", streamer_.statusJson(),
                     "Cache-Control: no-store\r\n");
        }
        else if (path == "/frame.jpg" || path == "/snapshot.jpg")
        {
            std::vector<uint8_t> frame;
            uint64_t frameId = 0;
            size_t width = 0;
            size_t height = 0;
            if (!streamer_.latestFrame(frame, frameId, width, height))
            {
                sendText(client, 503, "Service Unavailable", "text/plain; charset=utf-8", "No frame available\n",
                         "Cache-Control: no-store\r\nRetry-After: 1\r\n");
            }
            else
            {
                std::ostringstream extra;
                extra << "Cache-Control: no-store\r\n";
                extra << "X-Frame-Id: " << frameId << "\r\n";
                extra << "X-Image-Width: " << width << "\r\n";
                extra << "X-Image-Height: " << height << "\r\n";
                sendResponse(client, 200, "OK", "image/jpeg", frame, extra.str());
            }
        }
        else
        {
            sendText(client, 404, "Not Found", "text/plain; charset=utf-8", "Not found\n");
        }

        ::close(client);
    }

    AppConfig config_;
    CameraStreamer& streamer_;
};

AppConfig parseArgs(int argc, char** argv)
{
    AppConfig config;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--bind")
        {
            config.bindAddress = needValue("--bind");
        }
        else if (arg == "--port")
        {
            int port = std::stoi(needValue("--port"));
            if (port <= 0 || port > 65535)
            {
                throw std::runtime_error("port must be between 1 and 65535");
            }
            config.port = static_cast<uint16_t>(port);
        }
        else if (arg == "--serial")
        {
            config.serial = needValue("--serial");
        }
        else if (arg == "--jpeg-quality")
        {
            config.jpegQuality = parseUint32(needValue("--jpeg-quality"), "--jpeg-quality", 1, 100);
        }
        else if (arg == "--stream-width")
        {
            config.streamMaxWidth = parseUint32(needValue("--stream-width"), "--stream-width", 0, 10000);
        }
        else if (arg == "--stream-fps")
        {
            config.streamFps = parseUint32(needValue("--stream-fps"), "--stream-fps", 1, 60);
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: cam_website [--bind ADDRESS] [--port PORT] [--serial SERIAL]\n"
                         "                   [--jpeg-quality 1-100] [--stream-width PIXELS] [--stream-fps FPS]\n";
            std::exit(0);
        }
        else
        {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return config;
}
} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try
    {
        AppConfig config = parseArgs(argc, argv);
        CameraStreamer streamer(config.serial, config.jpegQuality, config.streamMaxWidth, config.streamFps);
        streamer.start();

        try
        {
            HttpServer server(config, streamer);
            server.run();
        }
        catch (...)
        {
            streamer.stop();
            throw;
        }

        streamer.stop();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
