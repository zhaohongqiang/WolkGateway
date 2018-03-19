#include "OutboundMessageHandler.h"
#include "connectivity/json/DeviceRegistrationProtocol.h"
#include "model/DeviceRegistrationRequest.h"
#include "model/DeviceRegistrationResponse.h"
#include "model/DeviceReregistrationResponse.h"
#include "model/Message.h"
#include "repository/DeviceRepository.h"
#include "repository/SQLiteDeviceRepository.h"
#include "service/DeviceRegistrationService.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
class PlatformOutboundMessageHandler : public wolkabout::OutboundMessageHandler
{
public:
    void addMessage(std::shared_ptr<wolkabout::Message> message) override { m_messages.push_back(message); }

    const std::vector<std::shared_ptr<wolkabout::Message>>& getMessages() const { return m_messages; }

private:
    std::vector<std::shared_ptr<wolkabout::Message>> m_messages;
};

class DeviceOutboundMessageHandler : public wolkabout::OutboundMessageHandler
{
public:
    void addMessage(std::shared_ptr<wolkabout::Message> message) override { m_messages.push_back(message); }

    const std::vector<std::shared_ptr<wolkabout::Message>>& getMessages() const { return m_messages; }

private:
    std::vector<std::shared_ptr<wolkabout::Message>> m_messages;
};

class DeviceRegistrationService : public ::testing::Test
{
public:
    void SetUp() override
    {
        deviceRepository = std::unique_ptr<wolkabout::SQLiteDeviceRepository>(
          new wolkabout::SQLiteDeviceRepository(DEVICE_REPOSITORY_PATH));
        platformOutboundMessageHandler =
          std::unique_ptr<PlatformOutboundMessageHandler>(new PlatformOutboundMessageHandler());
        deviceOutboundMessageHandler =
          std::unique_ptr<DeviceOutboundMessageHandler>(new DeviceOutboundMessageHandler());
        deviceRegistrationService =
          std::unique_ptr<wolkabout::DeviceRegistrationService>(new wolkabout::DeviceRegistrationService(
            GATEWAY_KEY, *deviceRepository, *platformOutboundMessageHandler, *deviceOutboundMessageHandler));
    }

    void TearDown() override { remove(DEVICE_REPOSITORY_PATH); }

    std::unique_ptr<wolkabout::SQLiteDeviceRepository> deviceRepository;
    std::unique_ptr<PlatformOutboundMessageHandler> platformOutboundMessageHandler;
    std::unique_ptr<DeviceOutboundMessageHandler> deviceOutboundMessageHandler;
    std::unique_ptr<wolkabout::DeviceRegistrationService> deviceRegistrationService;

    static constexpr const char* DEVICE_REPOSITORY_PATH = "testsDeviceRepository.db";
    static constexpr const char* GATEWAY_KEY = "gateway_key";
};
}    // namespace

TEST_F(
  DeviceRegistrationService,
  Given_ThatNoDeviceIsRegistered_When_DeviceOtherThanGatewayRequestsRegistration_Then_RegistrationRequestIsNotForwardedToPlatform)
{
    // Given
    // Intentionally left empty

    // When
    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);

    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_TRUE(platformOutboundMessageHandler->getMessages().empty());
}

TEST_F(DeviceRegistrationService,
       Given_ThatNoDeviceIsRegistered_When_GatewayRequestsRegistration_Then_RegistrationRequestIsForwardedToPlatform)
{
    // Given
    // Intentionally left empty

    // When
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::DeviceRegistrationRequest gatewayRegistrationRequest("Gateway name", GATEWAY_KEY, gatewayManifest);

    auto gatewayRegistrationRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                  gatewayRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(gatewayRegistrationRequestMessage);

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_ThatGatewayIsRegistered_When_DeviceOtherThanGatewayRequestsRegistration_Then_RegistrationRequestIsForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    // When
    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);

    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_RegisteredDevice_When_AlreadyRegisteredDeviceRequestsRegistration_Then_RegistrationRequestIsNotForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::Device device("Device name", deviceKey, deviceManifest);

    deviceRepository->save(device);

    // When
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_TRUE(platformOutboundMessageHandler->getMessages().empty());
}

TEST_F(
  DeviceRegistrationService,
  Given_ThatDeviceIsRegistered_When_AlreadyRegisteredDeviceRequestsRegistrationWithDifferentManifest_Then_RegistrationRequestIsForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::Device device("Device name", deviceKey, deviceManifest);

    deviceRepository->save(device);

    // When
    deviceManifest.addSensor(wolkabout::SensorManifest("Sensor name", "ref", "desc", "unit", "readingType",
                                                       wolkabout::SensorManifest::DataType::STRING, 1, 0, 1));
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_GatewayRegisteredWithJsonDataProtocol_When_DeviceWithProtocolOtherThanJsonRequestsRegistration_Then_RegistrationRequestNotIsForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    // When
    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonSingleProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Device name", deviceKey, deviceManifest);

    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_TRUE(platformOutboundMessageHandler->getMessages().empty());
}

TEST_F(
  DeviceRegistrationService,
  Given_GatewayRegisteredWithJsonDataProtocol_When_DeviceWithProtocolJsonRequestsRegistration_Then_RegistrationRequestIsForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    // When
    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::Device device("Device name", deviceKey, deviceManifest);

    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_GatewayRegistrationAwaitingPlatformResponse_When_DeviceIsSuccessfullyRegistered_Then_OnDeviceRegisteredCallbackIsInvoked)
{
    // Given
    std::string registeredDeviceKey;
    bool isRegisteredDeviceGateway;
    deviceRegistrationService->onDeviceRegistered([&](const std::string& deviceKey, bool isGateway) -> void {
        registeredDeviceKey = deviceKey;
        isRegisteredDeviceGateway = isGateway;
    });

    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::DeviceRegistrationRequest gatewayRegistrationRequest("Gateway name", GATEWAY_KEY, gatewayManifest);

    auto gatewayRegistrationRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                  gatewayRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(gatewayRegistrationRequestMessage);
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());

    // When
    wolkabout::DeviceRegistrationResponse gatewayRegistrationResponse(
      wolkabout::DeviceRegistrationResponse::Result::OK);
    auto gatewayRegistrationResponseMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationResponseMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                   gatewayRegistrationResponse);
    deviceRegistrationService->platformMessageReceived(gatewayRegistrationResponseMessage);

    // Then
    ASSERT_TRUE(GATEWAY_KEY == registeredDeviceKey);
    EXPECT_TRUE(isRegisteredDeviceGateway);
}

TEST_F(
  DeviceRegistrationService,
  Given_DeviceRegistrationAwaitingPlatformResponse_When_DeviceIsSuccessfullyRegistered_Then_OnDeviceRegisteredCallbackIsInvoked)
{
    // Given
    std::string registeredDeviceKey;
    bool isRegisteredDeviceGateway;
    deviceRegistrationService->onDeviceRegistered([&](const std::string& deviceKey, bool isGateway) -> void {
        registeredDeviceKey = deviceKey;
        isRegisteredDeviceGateway = isGateway;
    });

    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);

    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // When
    wolkabout::DeviceRegistrationResponse deviceRegistrationResponse(wolkabout::DeviceRegistrationResponse::Result::OK);
    auto deviceRegistrationResponseMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationResponseMessage(GATEWAY_KEY, deviceKey,
                                                                                   deviceRegistrationResponse);
    deviceRegistrationService->platformMessageReceived(deviceRegistrationResponseMessage);

    // Then
    ASSERT_TRUE(deviceKey == registeredDeviceKey);
    EXPECT_FALSE(isRegisteredDeviceGateway);
}

TEST_F(
  DeviceRegistrationService,
  Given_GatewayRegistrationAwaitingPlatformResponse_When_SuccessfulGatewayRegistrationResonseIsReceived_Then_RegisteredGatewayIsSavedToDeviceRepository)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::DeviceRegistrationRequest gatewayRegistrationRequest("Gateway name", GATEWAY_KEY, gatewayManifest);

    auto gatewayRegistrationRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                  gatewayRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(gatewayRegistrationRequestMessage);
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());

    // When
    wolkabout::DeviceRegistrationResponse gatewayRegistrationResponse(
      wolkabout::DeviceRegistrationResponse::Result::OK);
    auto gatewayRegistrationResponseMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationResponseMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                   gatewayRegistrationResponse);
    deviceRegistrationService->platformMessageReceived(gatewayRegistrationResponseMessage);

    // Then
    ASSERT_NE(nullptr, deviceRepository->findByDeviceKey(GATEWAY_KEY));
}

TEST_F(
  DeviceRegistrationService,
  Given_DeviceRegistrationAwaitingPlatformResponse_When_DeviceIsSuccessfullyRegistered_Then_RegisteredDeviceIsSavedToDeviceRepository)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    const std::string deviceKey("device_key");
    wolkabout::DeviceManifest deviceManifest("Manifest name", "Manifest description", "JsonProtocol", "DFUProtocol");
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);
    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);

    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);

    // When
    wolkabout::DeviceRegistrationResponse deviceRegistrationResponse(wolkabout::DeviceRegistrationResponse::Result::OK);
    auto deviceRegistrationResponseMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationResponseMessage(GATEWAY_KEY, deviceKey,
                                                                                   deviceRegistrationResponse);
    deviceRegistrationService->platformMessageReceived(deviceRegistrationResponseMessage);

    // Then
    ASSERT_NE(nullptr, deviceRepository->findByDeviceKey(deviceKey));
}

TEST_F(
  DeviceRegistrationService,
  Given_ThatGatewayIsNotRegisteredAndListOfDeviceRegistrationRequestsAndGatewayRegistrationRequest_When_GatewayIsRegistered_Then_PostponedDeviceRegistrationRequestsAreForwardedToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::DeviceRegistrationRequest gatewayRegistrationRequest("Gateway name", GATEWAY_KEY, gatewayManifest);

    auto gatewayRegistrationRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                  gatewayRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(gatewayRegistrationRequestMessage);
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());

    const std::string deviceKey("deviceKey");
    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::DeviceRegistrationRequest deviceRegistrationRequest("Device name", deviceKey, deviceManifest);

    auto deviceRegistrationRequestMessage = wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationRequestMessage(
      GATEWAY_KEY, deviceKey, deviceRegistrationRequest);
    deviceRegistrationService->deviceMessageReceived(deviceRegistrationRequestMessage);
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());

    // When
    wolkabout::DeviceRegistrationResponse gatewayRegistrationResponse(
      wolkabout::DeviceRegistrationResponse::Result::OK);
    auto gatewayRegistrationResponseMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceRegistrationResponseMessage(GATEWAY_KEY, GATEWAY_KEY,
                                                                                   gatewayRegistrationResponse);
    deviceRegistrationService->platformMessageReceived(gatewayRegistrationResponseMessage);

    // Then
    ASSERT_EQ(2, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_RegisteredGatewayAndChildDevice_When_DeviceReregistrationRequestIsReceived_Then_DeviceReregistrationRequestIsForwardedToChildDevice)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", "child_device_key", deviceManifest);
    deviceRepository->save(device);

    // When
    auto reregisterAllDevicesRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceReregistrationRequestForGateway(GATEWAY_KEY);
    deviceRegistrationService->platformMessageReceived(reregisterAllDevicesRequestMessage);

    // Then
    ASSERT_EQ(1, deviceOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_RegisteredGatewayAndChildDevice_When_DeviceReregistrationRequestIsReceived_Then_DeviceReregistrationRequestAcknowledgmentIsSentToPlatform)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", "child_device_key", deviceManifest);
    deviceRepository->save(device);

    // When
    auto reregisterAllDevicesRequestMessage =
      wolkabout::DeviceRegistrationProtocol::makeDeviceReregistrationRequestForGateway(GATEWAY_KEY);
    deviceRegistrationService->platformMessageReceived(reregisterAllDevicesRequestMessage);

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
}

TEST_F(
  DeviceRegistrationService,
  Given_SingleRegisteredChildDevice_When_DevicesOtherThanChildDeviceAreDeleted_Then_NoDeletionRequestIsSentToPlatform)
{
    // Given
    const std::string childDeviceKey = "child_device_key";

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", childDeviceKey, deviceManifest);
    deviceRepository->save(device);

    // When
    deviceRegistrationService->deleteDevicesOtherThan({childDeviceKey});

    // Then
    ASSERT_TRUE(platformOutboundMessageHandler->getMessages().empty());
}

TEST_F(DeviceRegistrationService,
       Given_SingleRegisteredChildDevice_When_ChildDeviceisDeleted_Then_DeletionRequestIsSentToPlatform)
{
    // Given
    const std::string childDeviceKey = "child_device_key";

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", childDeviceKey, deviceManifest);
    deviceRepository->save(device);

    // When
    deviceRegistrationService->deleteDevicesOtherThan({});

    // Then
    ASSERT_EQ(1, platformOutboundMessageHandler->getMessages().size());
    ASSERT_TRUE(wolkabout::DeviceRegistrationProtocol::isDeviceDeletionRequest(
      platformOutboundMessageHandler->getMessages().front()));
}

TEST_F(DeviceRegistrationService,
       Given_SingleRegisteredChildDevice_When_ChildDeviceisDeleted_Then_ChildDeviceIsDeletedFromDeviceRepository)
{
    // Given
    const std::string childDeviceKey = "child_device_key";

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", childDeviceKey, deviceManifest);
    deviceRepository->save(device);

    // When
    deviceRegistrationService->deleteDevicesOtherThan({});

    // Then
    ASSERT_FALSE(deviceRepository->containsDeviceWithKey(childDeviceKey));
}

TEST_F(DeviceRegistrationService,
       Given_RegisteredGatewayAndChildDevice_When_GatewayDeviceIsDeleted_Then_AllDevicesAreDeletedFromRepository)
{
    // Given
    wolkabout::DeviceManifest gatewayManifest("Gateway manifest name", "Gateway manifest description", "JsonProtocol",
                                              "DFUProtocol");
    wolkabout::Device gateway("Gateway", GATEWAY_KEY, gatewayManifest);
    deviceRepository->save(gateway);

    wolkabout::DeviceManifest deviceManifest("Device manifest name", "Device manifest description", "JsonProtocol",
                                             "DFUProtocol");
    wolkabout::Device device("Child device", "child_device_key", deviceManifest);
    deviceRepository->save(device);

    // When
    deviceRegistrationService->deleteDevicesOtherThan({});

    // Then
    ASSERT_TRUE(deviceRepository->findAllDeviceKeys()->empty());
}
