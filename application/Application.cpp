/*
 * Copyright 2018 WolkAbout Technology s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Configuration.h"
#include "Wolk.h"
#include "protocol/json/JsonGatewayDataProtocol.h"
#include "utilities/ConsoleLogger.h"
#include "utilities/FileSystemUtils.h"
#include "utilities/StringUtils.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
void setupLogger()
{
    auto logger = std::unique_ptr<wolkabout::ConsoleLogger>(new wolkabout::ConsoleLogger());
    logger->setLogLevel(wolkabout::LogLevel::INFO);
    wolkabout::Logger::setInstance(std::move(logger));
}

wolkabout::LogLevel parseLogLevel(const std::string& levelStr)
{
    const std::string str = wolkabout::StringUtils::toUpperCase(levelStr);
    const auto logLevel = [&]() -> wolkabout::LogLevel {
        if (str == "TRACE")
            return wolkabout::LogLevel::TRACE;
        else if (str == "DEBUG")
            return wolkabout::LogLevel::DEBUG;
        else if (str == "INFO")
            return wolkabout::LogLevel::INFO;
        else if (str == "WARN")
            return wolkabout::LogLevel::WARN;
        else if (str == "ERROR")
            return wolkabout::LogLevel::ERROR;

        throw std::logic_error("Unable to parse log level.");
    }();

    return logLevel;
}
}    // namespace

int firmwareVersionNumber = 1;

namespace example
{
class BasicUrlFileDownloader : public wolkabout::UrlFileDownloader
{
public:
    void download(const std::string& url, const std::string& downloadDirectory,
                  std::function<void(const std::string& filePath)> onSuccessCallback,
                  std::function<void(UrlFileDownloader::Error errorCode)> onFailCallback) override
    {
        if (wolkabout::FileSystemUtils::isFilePresent(url))
        {
            wolkabout::ByteArray content;
            if (wolkabout::FileSystemUtils::readBinaryFileContent(url, content))
            {
                static int firmwareFileNum = 0;
                const std::string filePath =
                  downloadDirectory + "/new_firmware_file" + std::to_string(++firmwareFileNum);
                if (wolkabout::FileSystemUtils::createBinaryFileWithContent(filePath, content))
                {
                    onSuccessCallback(filePath);
                    return;
                }
            }
        }

        onFailCallback(Error::UNSPECIFIED_ERROR);
    }

    void abort() override {}
};

class BasicFirmwareInstaller : public wolkabout::FirmwareInstaller
{
public:
    BasicFirmwareInstaller(int argc, char** argv, char** envp) : m_argc{argc}, m_argv{argv}, m_envp{envp} {}

    bool install(const std::string& firmwareFile) override
    {
        LOG(INFO) << "Installing gateway firmware: " << firmwareFile;

        //        unlink(m_argv[0]);

        //        std::string newExe = std::string(m_argv[0]) + "_dfu";

        //        std::rename(firmwareFile.c_str(), newExe.c_str());

        //        chmod(newExe.c_str(), S_IRWXU|S_IRWXG|S_IRWXO);

        //        char * argv[] = {(char*)newExe.c_str(), nullptr};
        //        char * envp[] = {nullptr};

        if (m_argc > 3)
        {
            int version = std::stoi(m_argv[3]);
            ++version;
            m_argv[3] = (char*)std::to_string(version).c_str();
        }

        auto ret = execve(m_argv[0], m_argv, m_envp);

        LOG(ERROR) << std::strerror(errno);

        return ret != -1;
    }

private:
    int m_argc;
    char** m_argv;
    char** m_envp;
};
}    // namespace example

int main(int argc, char** argv, char** envp)
{
    setupLogger();

    if (argc < 2)
    {
        LOG(ERROR) << "WolkGateway Application: Usage -  " << argv[0] << " [gatewayConfigurationFilePath] [logLevel]";
        return -1;
    }

    wolkabout::GatewayConfiguration gatewayConfiguration;
    try
    {
        gatewayConfiguration = wolkabout::GatewayConfiguration::fromJson(argv[1]);
    }
    catch (std::logic_error& e)
    {
        LOG(ERROR) << "WolkGateway Application: Unable to parse gateway configuration file. Reason: " << e.what();
        return -1;
    }

    if (argc > 2)
    {
        const std::string logLevelStr{argv[2]};
        try
        {
            wolkabout::LogLevel level = parseLogLevel(logLevelStr);
            wolkabout::Logger::getInstance()->setLogLevel(level);
        }
        catch (std::logic_error& e)
        {
            LOG(ERROR) << "WolkGateway Application: " << e.what();
        }
    }

    if (argc > 3)
    {
        firmwareVersionNumber = std::stoi(argv[3]);
    }

    std::string firmwareVersion = std::to_string(firmwareVersionNumber) + ".0.0";

    auto dataProtocol = std::unique_ptr<wolkabout::JsonGatewayDataProtocol>(new wolkabout::JsonGatewayDataProtocol());

    wolkabout::ActuatorManifest am{"name", "REF", wolkabout::DataType::NUMERIC, ""};

    auto installer = std::make_shared<example::BasicFirmwareInstaller>(argc, argv, envp);
    auto urlDownloader = std::make_shared<example::BasicUrlFileDownloader>();

    wolkabout::Device device(
      gatewayConfiguration.getKey(), gatewayConfiguration.getPassword(),
      wolkabout::DeviceManifest{"EmptyManifest", "", dataProtocol->getName(), "DFU", {}, {}, {}, {am}});
    auto builder = wolkabout::Wolk::newBuilder(device)
                     .withDataProtocol(std::move(dataProtocol))
                     .gatewayHost(gatewayConfiguration.getLocalMqttUri())
                     .platformHost(gatewayConfiguration.getPlatformMqttUri())
                     .withFirmwareUpdate(firmwareVersion, installer, ".", 10 * 1024 * 1024, 1024, urlDownloader);

    if (gatewayConfiguration.getKeepAliveEnabled() && !gatewayConfiguration.getKeepAliveEnabled().value())
    {
        builder.withoutKeepAlive();
    }

    if (gatewayConfiguration.getPlatformTrustStore())
    {
        builder.platformTrustStore(gatewayConfiguration.getPlatformTrustStore().value());
    }

    std::unique_ptr<wolkabout::Wolk> wolk = builder.build();

    wolk->connect();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return 0;
}
