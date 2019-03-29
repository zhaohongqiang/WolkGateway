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
#include <random>
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
class ActuatorHandler
{
public:
    virtual ~ActuatorHandler() = default;
    virtual std::string getValue() = 0;
    virtual void setValue(std::string value) = 0;
};

template <class T> class ActuatorTemplateHandler : public ActuatorHandler
{
public:
    void setValue(std::string value) override
    {
        try
        {
            m_value = std::stod(value);
        }
        catch (...)
        {
        }
    }

    std::string getValue() override { return std::to_string(m_value); }

private:
    T m_value;
};

template <> class ActuatorTemplateHandler<bool> : public ActuatorHandler
{
public:
    void setValue(std::string value) override { m_value = value == "true"; }

    std::string getValue() override { return m_value ? "true" : "false"; }

private:
    bool m_value;
};

template <> class ActuatorTemplateHandler<std::string> : public ActuatorHandler
{
public:
    void setValue(std::string value) override { m_value = value; }

    std::string getValue() override { return m_value; }

private:
    std::string m_value;
};

class BasicUrlFileDownloader : public wolkabout::UrlFileDownloader
{
public:
    void download(
      const std::string& url, const std::string& downloadDirectory,
      std::function<void(const std::string& url, const std::string& fileName, const std::string& filePath)>
        onSuccessCallback,
      std::function<void(const std::string& url, wolkabout::FileTransferError errorCode)> onFailCallback) override
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
                    onSuccessCallback(url, "new_firmware_file", filePath);
                    return;
                }
            }
        }

        onFailCallback(url, wolkabout::FileTransferError::UNSPECIFIED_ERROR);
    }

    void abort(const std::string& url) override {}
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

    wolkabout::GatewayConfiguration gatewayConfiguration = [&] {
        try
        {
            return wolkabout::GatewayConfiguration::fromJson(argv[1]);
        }
        catch (std::logic_error& e)
        {
            LOG(ERROR) << "WolkGateway Application: Unable to parse gateway configuration file. Reason: " << e.what();
            std::exit(-1);
        }
    }();

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

    std::map<std::string, std::shared_ptr<example::ActuatorHandler>> handlers;
    for (const auto& actuator : gatewayConfiguration.getDevice().getTemplate().getActuators())
    {
        std::shared_ptr<example::ActuatorHandler> handler;
        if (actuator.getReadingTypeName() == "SWITCH(ACTUATOR)")
        {
            handler.reset(new example::ActuatorTemplateHandler<bool>());
        }
        else if (actuator.getReadingTypeName() == "COUNT(ACTUATOR)")
        {
            handler.reset(new example::ActuatorTemplateHandler<double>());
        }
        else
        {
            handler.reset(new example::ActuatorTemplateHandler<std::string>());
        }

        handlers[actuator.getReference()] = handler;
    }

    std::vector<wolkabout::ConfigurationItem> localConfiguration;
    for (const auto& conf : gatewayConfiguration.getDevice().getTemplate().getConfigurations())
    {
        localConfiguration.push_back(wolkabout::ConfigurationItem{
          std::vector<std::string>(conf.getSize(), conf.getDefaultValue()), conf.getReference()});
    }

    std::string firmwareVersion = std::to_string(firmwareVersionNumber) + ".0.0";

    auto installer = std::make_shared<example::BasicFirmwareInstaller>(argc, argv, envp);
    auto urlDownloader = std::make_shared<example::BasicUrlFileDownloader>();

    auto builder =
      wolkabout::Wolk::newBuilder(gatewayConfiguration.getDevice())
        .actuationHandler([&](const std::string& reference, const std::string& value) -> void {
            LOG(INFO) << "Actuation request received -  Reference: " << reference << " value: " << value;

            auto it = handlers.find(reference);
            if (it != handlers.end())
            {
                it->second->setValue(value);
            }
        })
        .actuatorStatusProvider([&](const std::string& reference) -> wolkabout::ActuatorStatus {
            auto it = handlers.find(reference);
            if (it != handlers.end())
            {
                return wolkabout::ActuatorStatus(it->second->getValue(), wolkabout::ActuatorStatus::State::READY);
            }

            return wolkabout::ActuatorStatus("", wolkabout::ActuatorStatus::State::ERROR);
        })
        .configurationHandler(
          [&](const std::vector<wolkabout::ConfigurationItem>& configuration) { localConfiguration = configuration; })
        .configurationProvider([&]() -> std::vector<wolkabout::ConfigurationItem> { return localConfiguration; })
        .gatewayHost(gatewayConfiguration.getLocalMqttUri())
        .platformHost(gatewayConfiguration.getPlatformMqttUri());

    if (gatewayConfiguration.getKeepAliveEnabled() && !gatewayConfiguration.getKeepAliveEnabled().value())
    {
        builder.withoutKeepAlive();
    }

    if (gatewayConfiguration.getPlatformTrustStore())
    {
        builder.platformTrustStore(gatewayConfiguration.getPlatformTrustStore().value());
    }

    if (!gatewayConfiguration.getDevice().getTemplate().getFirmwareUpdateType().empty() &&
        gatewayConfiguration.getDevice().getFirmwareUpdate() &&
        gatewayConfiguration.getDevice().getFirmwareUpdate().value())
    {
        builder.withFirmwareUpdate(firmwareVersion, installer);
    }

    if (gatewayConfiguration.getDevice().getUrlDownload() && gatewayConfiguration.getDevice().getUrlDownload().value())
    {
        builder.withUrlFileDownload(urlDownloader);
    }

    std::unique_ptr<wolkabout::Wolk> wolk = builder.build();

    wolk->connect();

    std::random_device rd;
    std::mt19937 mt(rd());

    while (true)
    {
        for (const auto& sensor : gatewayConfiguration.getDevice().getTemplate().getSensors())
        {
            std::vector<int> values;

            if (gatewayConfiguration.getValueGenerator() == wolkabout::ValueGenerator::INCEREMENTAL)
            {
                static int value = 0;
                int size = 1;
                if (!sensor.getDescription().empty())
                {
                    // get sensor size from description as the size param is removed
                    try
                    {
                        size = std::stoi(sensor.getDescription());
                    }
                    catch (...)
                    {
                    }
                }
                for (int i = 0; i < size; ++i)
                {
                    values.push_back(++value);
                }
            }
            else
            {
                std::uniform_int_distribution<int> dist(sensor.getMinimum(), sensor.getMaximum());

                int size = 1;
                if (!sensor.getDescription().empty())
                {
                    // get sensor size from description as the size param is removed
                    try
                    {
                        size = std::stoi(sensor.getDescription());
                    }
                    catch (...)
                    {
                    }
                }
                for (int i = 0; i < size; ++i)
                {
                    int rand_num = dist(mt);
                    values.push_back(rand_num);
                }
            }

            wolk->addSensorReading(sensor.getReference(), values);
        }

        wolk->publish();

        std::this_thread::sleep_for(std::chrono::milliseconds(gatewayConfiguration.getInterval()));
    }

    return 0;
}
