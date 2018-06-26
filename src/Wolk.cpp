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

#include "Wolk.h"
#include "InboundMessageHandler.h"
#include "WolkBuilder.h"
#include "connectivity/ConnectivityService.h"
#include "model/DetailedDevice.h"
#include "repository/DeviceRepository.h"
#include "service/DataService.h"
#include "service/KeepAliveService.h"
#include "service/PublishingService.h"
#include "utilities/Logger.h"

#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace
{
const unsigned RECONNECT_DELAY_MSEC = 2000;
}

namespace wolkabout
{
const constexpr std::chrono::seconds Wolk::KEEP_ALIVE_INTERVAL;

WolkBuilder Wolk::newBuilder(Device device)
{
    return WolkBuilder(device);
}

void Wolk::connect()
{
    connectToPlatform();
    connectToDevices();
}

void Wolk::disconnect()
{
    addToCommandBuffer([=]() -> void { m_platformConnectivityService->disconnect(); });
    addToCommandBuffer([=]() -> void { m_deviceConnectivityService->disconnect(); });
}

Wolk::Wolk(Device device) : m_device{device}
{
    m_commandBuffer = std::unique_ptr<CommandBuffer>(new CommandBuffer());
}

void Wolk::addToCommandBuffer(std::function<void()> command)
{
    m_commandBuffer->pushCommand(std::make_shared<std::function<void()>>(command));
}

unsigned long long Wolk::currentRtc()
{
    auto duration = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::seconds>(duration).count());
}

void Wolk::notifyPlatformConnected()
{
    m_platformPublisher->connected();

    if (m_keepAliveService)
    {
        m_keepAliveService->connected();
    }
}

void Wolk::notifyPlatformDisonnected()
{
    m_platformPublisher->disconnected();

    if (m_keepAliveService)
    {
        m_keepAliveService->disconnected();
    }
}

void Wolk::notifyDevicesConnected()
{
    m_devicePublisher->connected();

    m_deviceStatusService->connected();
}

void Wolk::notifyDevicesDisonnected()
{
    m_devicePublisher->disconnected();

    m_deviceStatusService->disconnected();
}

void Wolk::connectToPlatform()
{
    addToCommandBuffer([=]() -> void {
        if (m_platformConnectivityService->connect())
        {
            notifyPlatformConnected();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MSEC));
            connectToPlatform();
        }
    });
}

void Wolk::connectToDevices()
{
    addToCommandBuffer([=]() -> void {
        if (m_deviceConnectivityService->connect())
        {
            notifyDevicesConnected();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MSEC));
            connectToDevices();
        }
    });
}

void Wolk::routePlatformData(const std::string& protocol, std::shared_ptr<Message> message)
{
    std::lock_guard<decltype(m_lock)> lg{m_lock};

    auto it = m_dataServices.find(protocol);
    if (it != m_dataServices.end())
    {
        std::get<0>(it->second)->platformMessageReceived(message);
    }
    else
    {
        LOG(WARN) << "Data service not found for protocol: " << protocol;
    }
}

void Wolk::routeDeviceData(const std::string& protocol, std::shared_ptr<Message> message)
{
    std::lock_guard<decltype(m_lock)> lg{m_lock};

    auto it = m_dataServices.find(protocol);
    if (it != m_dataServices.end())
    {
        std::get<0>(it->second)->deviceMessageReceived(message);
    }
    else
    {
        LOG(WARN) << "Data service not found for protocol: " << protocol;
    }
}

void Wolk::gatewayRegistered()
{
    auto gatewayDevice = m_deviceRepository->findByDeviceKey(m_device.getKey());
    if (!gatewayDevice)
    {
        LOG(WARN) << "Gateway device not found in repository";
        return;
    }

    const std::string gatewayProtocol = gatewayDevice->getManifest().getProtocol();

    if (gatewayProtocol.empty())
    {
        LOG(WARN) << "Gateway protocol not set";
        return;
    }

    setupGatewayListeners(gatewayProtocol);
}

void Wolk::setupGatewayListeners(const std::string& protocol)
{
    std::lock_guard<decltype(m_lock)> lg{m_lock};

    auto it = m_dataServices.find(protocol);
    if (it != m_dataServices.end())
    {
        m_deviceStatusService->setGatewayModuleConnectionStatusListener(std::get<0>(it->second));
    }
    else
    {
        LOG(WARN) << "Message protocol not found for gateway";
    }
}

void Wolk::registerDataProtocol(std::shared_ptr<GatewayDataProtocol> protocol)
{
    std::lock_guard<decltype(m_lock)> lg{m_lock};

    if (auto it = m_dataServices.find(protocol->getName()) != m_dataServices.end())
    {
        LOG(INFO) << "Data protocol already registered";
        return;
    }

    auto dataService = std::make_shared<DataService>(m_device.getKey(), *protocol, *m_deviceRepository,
                                                     *m_platformPublisher, *m_devicePublisher);

    auto protocolResolver =
      std::make_shared<ChannelProtocolResolver>(*protocol, *m_deviceRepository,
                                                [&](const std::string& protocolName, std::shared_ptr<Message> message) {
                                                    routePlatformData(protocolName, message);
                                                },
                                                [&](const std::string& protocolName, std::shared_ptr<Message> message) {
                                                    routeDeviceData(protocolName, message);
                                                });

    m_dataServices[protocol->getName()] = std::make_tuple(dataService, protocol, protocolResolver);

    m_inboundDeviceMessageHandler->addListener(protocolResolver);
    m_inboundPlatformMessageHandler->addListener(protocolResolver);
}
}    // namespace wolkabout