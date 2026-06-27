#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <ctime>
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

void writeLe16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void writeLe32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
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

std::vector<uint8_t> makeBmpFromRgb8(const uint8_t* rgb, size_t width, size_t height, size_t stride)
{
    if (width == 0 || height == 0 || width > 65535 || height > 65535)
    {
        throw std::runtime_error("invalid frame dimensions");
    }

    const uint32_t rowSize = static_cast<uint32_t>(((width * 3) + 3) & ~static_cast<size_t>(3));
    const uint32_t pixelSize = rowSize * static_cast<uint32_t>(height);
    const uint32_t fileSize = 54 + pixelSize;

    std::vector<uint8_t> out;
    out.reserve(fileSize);

    out.push_back('B');
    out.push_back('M');
    writeLe32(out, fileSize);
    writeLe16(out, 0);
    writeLe16(out, 0);
    writeLe32(out, 54);
    writeLe32(out, 40);
    writeLe32(out, static_cast<uint32_t>(width));
    writeLe32(out, static_cast<uint32_t>(height));
    writeLe16(out, 1);
    writeLe16(out, 24);
    writeLe32(out, 0);
    writeLe32(out, pixelSize);
    writeLe32(out, 2835);
    writeLe32(out, 2835);
    writeLe32(out, 0);
    writeLe32(out, 0);

    const size_t padding = rowSize - (width * 3);
    const uint8_t pad[3] = {0, 0, 0};

    for (size_t y = 0; y < height; ++y)
    {
        const uint8_t* src = rgb + ((height - 1 - y) * stride);
        for (size_t x = 0; x < width; ++x)
        {
            out.push_back(src[x * 3 + 2]);
            out.push_back(src[x * 3 + 1]);
            out.push_back(src[x * 3 + 0]);
        }
        out.insert(out.end(), pad, pad + padding);
    }

    return out;
}

struct AppConfig
{
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 8080;
    std::string serial;
};

class CameraStreamer
{
  public:
    explicit CameraStreamer(std::string serial) : serialFilter_(std::move(serial)) {}

    void start()
    {
        worker_ = std::thread(&CameraStreamer::captureLoop, this);
    }

    void stop()
    {
        running_ = false;
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    bool latestFrame(std::vector<uint8_t>& frame, uint64_t& frameId, size_t& width, size_t& height) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (latestBmp_.empty())
        {
            return false;
        }
        frame = latestBmp_;
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
        out << "\"camera_count\":" << cameraCount_ << ",";
        out << "\"serial\":\"" << jsonEscape(serial_) << "\",";
        out << "\"model\":\"" << jsonEscape(model_) << "\",";
        out << "\"access_status\":\"" << jsonEscape(accessStatus_) << "\",";
        out << "\"last_error\":\"" << jsonEscape(lastError_) << "\"";
        out << "}";
        return out.str();
    }

  private:
    void setStatus(bool connected, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = connected;
        lastError_ = error;
        if (!connected)
        {
            width_ = 0;
            height_ = 0;
            latestBmp_.clear();
        }
    }

    CameraPtr chooseCamera(CameraList& camList)
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

        if (serialFilter_.empty())
        {
            return camList.GetByIndex(0);
        }

        for (unsigned int i = 0; i < camList.GetSize(); ++i)
        {
            CameraPtr cam = camList.GetByIndex(i);
            INodeMap& tl = cam->GetTLDeviceNodeMap();
            if (readStringNode(tl, "DeviceSerialNumber") == serialFilter_)
            {
                return cam;
            }
        }

        return nullptr;
    }

    void captureOnce()
    {
        SystemPtr system = System::GetInstance();
        CameraList camList = system->GetCameras();

        CameraPtr cam = chooseCamera(camList);
        if (cam == nullptr)
        {
            std::ostringstream error;
            error << "No matching camera found";
            if (!serialFilter_.empty())
            {
                error << " for serial " << serialFilter_;
            }
            setStatus(false, error.str());
            camList.Clear();
            system->ReleaseInstance();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            return;
        }

        INodeMap& tlDevice = cam->GetTLDeviceNodeMap();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            serial_ = readStringNode(tlDevice, "DeviceSerialNumber");
            model_ = readStringNode(tlDevice, "DeviceModelName");
            accessStatus_ = readEnumNode(tlDevice, "DeviceAccessStatus");
            lastError_.clear();
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

            cam->BeginAcquisition();
            acquiring = true;
            setStatus(true, "");

            while (running_ && g_running)
            {
                ImagePtr image = cam->GetNextImage(1000);
                if (image->IsIncomplete())
                {
                    std::string description = Image::GetImageStatusDescription(image->GetImageStatus());
                    image->Release();
                    setStatus(true, "Incomplete image: " + description);
                    continue;
                }

                ImagePtr rgb = processor.Convert(image, PixelFormat_RGB8);
                std::vector<uint8_t> bmp = makeBmpFromRgb8(
                    static_cast<const uint8_t*>(rgb->GetData()), rgb->GetWidth(), rgb->GetHeight(), rgb->GetStride());

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    latestBmp_ = std::move(bmp);
                    frameId_ = image->GetFrameID();
                    width_ = rgb->GetWidth();
                    height_ = rgb->GetHeight();
                    connected_ = true;
                    lastError_.clear();
                }

                image->Release();
            }
        }
        catch (const Spinnaker::Exception& e)
        {
            setStatus(false, e.what());
        }
        catch (const std::exception& e)
        {
            setStatus(false, e.what());
        }

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

        if (running_ && g_running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
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

    std::string serialFilter_;
    std::atomic<bool> running_{true};
    std::thread worker_;

    mutable std::mutex mutex_;
    std::vector<uint8_t> latestBmp_;
    uint64_t frameId_ = 0;
    size_t width_ = 0;
    size_t height_ = 0;
    size_t cameraCount_ = 0;
    bool connected_ = false;
    std::string serial_;
    std::string model_;
    std::string accessStatus_;
    std::string lastError_ = "Starting";
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
    let paused = false;
    let delay = 35;

    function setStatus(s) {
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

    function nextFrame() {
      if (paused) return;
      img.src = `/frame.bmp?t=${Date.now()}`;
    }

    img.onload = () => {
      delay = 35;
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

    pollStatus();
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

        size_t query = path.find('?');
        if (query != std::string::npos)
        {
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
        else if (path == "/frame.bmp" || path == "/snapshot.bmp")
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
                sendResponse(client, 200, "OK", "image/bmp", frame, extra.str());
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
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: cam_website [--bind ADDRESS] [--port PORT] [--serial SERIAL]\n";
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
        CameraStreamer streamer(config.serial);
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
