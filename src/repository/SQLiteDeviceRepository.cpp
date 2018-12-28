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

#include "repository/SQLiteDeviceRepository.h"
#include "model/ActuatorManifest.h"
#include "model/AlarmManifest.h"
#include "model/ConfigurationManifest.h"
#include "model/DataType.h"
#include "model/DetailedDevice.h"
#include "model/DeviceManifest.h"

#include "utilities/Logger.h"

#include "Poco/Crypto/DigestEngine.h"
#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/SQLite/SQLiteException.h"
#include "Poco/Data/Session.h"
#include "Poco/Data/Statement.h"
#include "Poco/String.h"
#include "Poco/Types.h"

#include <memory>
#include <mutex>
#include <regex>
#include <string>

namespace wolkabout
{
using namespace Poco::Data::Keywords;
using Poco::Data::Session;
using Poco::Data::Statement;

SQLiteDeviceRepository::SQLiteDeviceRepository(const std::string& connectionString)
{
    Poco::Data::SQLite::Connector::registerConnector();
    m_session = std::unique_ptr<Session>(new Session(Poco::Data::SQLite::Connector::KEY, connectionString));

    Statement statement(*m_session);

    // Alarm manifest
    statement << "CREATE TABLE IF NOT EXISTS alarm_manifest (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, reference "
                 "TEXT, name TEXT, severity TEXT, message TEXT, description TEXT, device_manifest_id INTEGER, "
                 "FOREIGN KEY(device_manifest_id) REFERENCES device_manifest(id) ON DELETE CASCADE);";

    // Actuator manifest
    statement << "CREATE TABLE IF NOT EXISTS actuator_manifest (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                 "reference TEXT, name TEXT, description TEXT, unit_symbol TEXT, reading_type TEXT, data_type TEXT, "
                 "precision INTEGER, minimum REAL, maximum REAL, delimiter TEXT, device_manifest_id INTEGER, "
                 "FOREIGN KEY(device_manifest_id) REFERENCES device_manifest(id) ON DELETE CASCADE);";

    statement << "CREATE TABLE IF NOT EXISTS actuator_label (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, label "
                 "TEXT, actuator_manifest_id INTEGER, "
                 "FOREIGN KEY(actuator_manifest_id) REFERENCES actuator_manifest(id) ON DELETE CASCADE);";

    // Sensor manifest
    statement << "CREATE TABLE IF NOT EXISTS sensor_manifest (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                 "reference TEXT, name TEXT, description TEXT, unit_symbol TEXT, reading_type TEXT, data_type TEXT, "
                 "precision INTEGER, minimum REAL, maximum REAL, delimiter TEXT, device_manifest_id INTEGER, "
                 "FOREIGN KEY(device_manifest_id) REFERENCES device_manifest(id) ON DELETE CASCADE);";

    statement << "CREATE TABLE IF NOT EXISTS sensor_label (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, label TEXT, "
                 "sensor_manifest_id INTEGER, "
                 "FOREIGN KEY(sensor_manifest_id) REFERENCES sensor_manifest(id) ON DELETE CASCADE);";

    // Configuration manifest
    statement << "CREATE TABLE IF NOT EXISTS configuration_manifest (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                 "reference TEXT, name TEXT, description TEXT, "
                 "data_type TEXT, minimum REAL, maximum REAL, delimiter TEXT, "
                 "default_value TEXT, device_manifest_id INTEGER, "
                 "FOREIGN KEY(device_manifest_id) REFERENCES device_manifest(id) ON DELETE CASCADE);";

    statement
      << "CREATE TABLE IF NOT EXISTS configuration_label (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, label TEXT, "
         "configuration_manifest_id INTEGER, "
         "FOREIGN KEY(configuration_manifest_id) REFERENCES configuration_manifest(id) ON DELETE CASCADE);";

    // Device manifest
    statement << "CREATE TABLE IF NOT EXISTS device_manifest (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, name "
                 "TEXT, description TEXT, protocol TEXT, firmware_update_protocol TEXT, sha256 TEXT);";

    // Device
    statement
      << "CREATE TABLE IF NOT EXISTS device (key TEXT PRIMARY KEY, name TEXT, device_manifest_id INTEGER NOT NULL, "
         "FOREIGN KEY(device_manifest_id) REFERENCES device_manifest(id));";

    statement << "PRAGMA foreign_keys=on;";

    statement.execute();
}

void SQLiteDeviceRepository::save(const DetailedDevice& device)
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    try
    {
        Statement statement(*m_session);

        Poco::UInt64 devicesWithGivenKeyCount;
        statement << "SELECT count(*) FROM device WHERE device.key=?;", useRef(device.getKey()),
          into(devicesWithGivenKeyCount), now;

        if (devicesWithGivenKeyCount != 0)
        {
            update(device);
            return;
        }

        const std::string deviceManifestSha256 = calculateSha256(device.getManifest());
        statement.reset(*m_session);
        Poco::UInt64 matchingDeviceManifestsCount;
        statement << "SELECT count(*) FROM device_manifest WHERE sha256=?;", useRef(deviceManifestSha256),
          into(matchingDeviceManifestsCount), now;
        if (matchingDeviceManifestsCount != 0)
        {
            // Equivalent manifest exists
            statement.reset(*m_session);
            statement << "INSERT INTO device SELECT ?, ?, id FROM device_manifest WHERE device_manifest.sha256=?;",
              useRef(device.getKey()), useRef(device.getName()), useRef(deviceManifestSha256), now;
            return;
        }

        // Create new device manifest
        statement.reset(*m_session);
        statement << "BEGIN TRANSACTION;";
        statement
          << "INSERT INTO device_manifest(name, description, protocol, firmware_update_protocol, sha256) VALUES(?, "
             "?, ?, ?, ?);",
          useRef(device.getManifest().getName()), useRef(device.getManifest().getDescription()),
          useRef(device.getManifest().getProtocol()), useRef(device.getManifest().getFirmwareUpdateType()),
          useRef(deviceManifestSha256);

        Poco::UInt64 deviceManifestId;
        statement << "SELECT last_insert_rowid();", into(deviceManifestId);

        // Alarm manifests
        for (const wolkabout::AlarmManifest& alarmManifest : device.getManifest().getAlarms())
        {
            const auto severity = [&]() -> std::string {
                if (alarmManifest.getSeverity() == AlarmManifest::AlarmSeverity::ALERT)
                {
                    return "ALERT";
                }
                else if (alarmManifest.getSeverity() == AlarmManifest::AlarmSeverity::CRITICAL)
                {
                    return "CRITICAL";
                }
                else if (alarmManifest.getSeverity() == AlarmManifest::AlarmSeverity::ERROR)
                {
                    return "ERROR";
                }

                return "";
            }();

            statement
              << "INSERT INTO alarm_manifest(reference, name, severity, message, description, device_manifest_id) "
                 "VALUES(?, ?, ?, ?, ?, ?);",
              useRef(alarmManifest.getReference()), useRef(alarmManifest.getName()), bind(severity),
              useRef(alarmManifest.getMessage()), useRef(alarmManifest.getDescription()), useRef(deviceManifestId);
        }

        // Actuator manifests
        for (const wolkabout::ActuatorManifest& actuatorManifest : device.getManifest().getActuators())
        {
            const auto dataType = [&]() -> std::string {
                if (actuatorManifest.getDataType() == DataType::BOOLEAN)
                {
                    return "BOOLEAN";
                }
                else if (actuatorManifest.getDataType() == DataType::NUMERIC)
                {
                    return "NUMERIC";
                }
                else if (actuatorManifest.getDataType() == DataType::STRING)
                {
                    return "STRING";
                }

                return "";
            }();

            const auto precision = Poco::UInt32(actuatorManifest.getPrecision());
            const auto minimum = actuatorManifest.getMinimum();
            const auto maximum = actuatorManifest.getMaximum();
            statement
              << "INSERT INTO actuator_manifest(reference, name, description, unit_symbol, reading_type, data_type, "
                 "precision, minimum, maximum, delimiter, device_manifest_id) "
                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
              useRef(actuatorManifest.getReference()), useRef(actuatorManifest.getName()),
              useRef(actuatorManifest.getDescription()), useRef(actuatorManifest.getUnitSymbol()),
              useRef(actuatorManifest.getReadingTypeName()), bind(dataType), bind(precision), bind(minimum),
              bind(maximum), useRef(actuatorManifest.getDelimiter()), useRef(deviceManifestId);

            for (const std::string& label : actuatorManifest.getLabels())
            {
                statement << "INSERT INTO actuator_label SELECT NULL, ?, id FROM actuator_manifest WHERE "
                             "actuator_manifest.reference=? AND actuator_manifest.device_manifest_id=?;",
                  useRef(label), useRef(actuatorManifest.getReference()), useRef(deviceManifestId);
            }
        }

        // Sensor manifests
        for (const wolkabout::SensorManifest& sensorManifest : device.getManifest().getSensors())
        {
            const auto dataType = [&]() -> std::string {
                if (sensorManifest.getDataType() == DataType::BOOLEAN)
                {
                    return "BOOLEAN";
                }
                else if (sensorManifest.getDataType() == DataType::NUMERIC)
                {
                    return "NUMERIC";
                }
                else if (sensorManifest.getDataType() == DataType::STRING)
                {
                    return "STRING";
                }

                return "";
            }();

            const auto precision = Poco::UInt32(sensorManifest.getPrecision());
            const auto minimum = sensorManifest.getMinimum();
            const auto maximum = sensorManifest.getMaximum();
            statement
              << "INSERT INTO sensor_manifest(reference, name, description, unit_symbol, reading_type, data_type, "
                 "precision, minimum, maximum, delimiter, device_manifest_id) "
                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
              useRef(sensorManifest.getReference()), useRef(sensorManifest.getName()),
              useRef(sensorManifest.getDescription()), useRef(sensorManifest.getUnitSymbol()),
              useRef(sensorManifest.getReadingTypeName()), bind(dataType), bind(precision), bind(minimum),
              bind(maximum), useRef(sensorManifest.getDelimiter()), useRef(deviceManifestId);

            for (const std::string& label : sensorManifest.getLabels())
            {
                statement << "INSERT INTO sensor_label SELECT NULL, ?, id FROM sensor_manifest WHERE "
                             "sensor_manifest.reference=? AND sensor_manifest.device_manifest_id=?;",
                  useRef(label), useRef(sensorManifest.getReference()), useRef(deviceManifestId);
            }
        }

        // Configuration manifests
        for (const wolkabout::ConfigurationManifest& configurationManifest : device.getManifest().getConfigurations())
        {
            const auto dataType = [&]() -> std::string {
                if (configurationManifest.getDataType() == DataType::BOOLEAN)
                {
                    return "BOOLEAN";
                }
                else if (configurationManifest.getDataType() == DataType::NUMERIC)
                {
                    return "NUMERIC";
                }
                else if (configurationManifest.getDataType() == DataType::STRING)
                {
                    return "STRING";
                }

                return "";
            }();

            const auto minimum = configurationManifest.getMinimum();
            const auto maximum = configurationManifest.getMaximum();

            statement << "INSERT INTO configuration_manifest(reference, name, description, data_type, minimum, "
                         "maximum, delimiter, default_value, device_manifest_id)"
                         "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
              useRef(configurationManifest.getReference()), useRef(configurationManifest.getName()),
              useRef(configurationManifest.getDescription()), bind(dataType), bind(minimum), bind(maximum),
              useRef(configurationManifest.getDelimiter()), useRef(configurationManifest.getDefaultValue()),
              useRef(deviceManifestId);

            for (const std::string& label : configurationManifest.getLabels())
            {
                statement << "INSERT INTO configuration_label SELECT NULL, ?, id FROM configuration_manifest WHERE "
                             "configuration_manifest.reference=? AND configuration_manifest.device_manifest_id=?;",
                  useRef(label), useRef(configurationManifest.getReference()), useRef(deviceManifestId);
            }
        }

        // Device
        statement << "INSERT INTO device(key, name, device_manifest_id) VALUES(?, ?, ?);", useRef(device.getKey()),
          useRef(device.getName()), useRef(deviceManifestId);
        statement << "COMMIT;", now;
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error saving device with key " << device.getKey();
    }
}

void SQLiteDeviceRepository::remove(const std::string& deviceKey)
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    try
    {
        Statement statement(*m_session);

        Poco::UInt64 deviceManifestId;
        statement << "SELECT device_manifest_id FROM device WHERE device.key=?;", useRef(deviceKey),
          into(deviceManifestId);
        if (statement.execute() == 0)
        {
            return;
        }

        statement.reset(*m_session);
        Poco::UInt64 numberOfDevicesReferencingManifest;
        statement << "SELECT count(*) FROM device WHERE device_manifest_id=?;", useRef(deviceManifestId),
          into(numberOfDevicesReferencingManifest), now;
        if (numberOfDevicesReferencingManifest != 1)
        {
            statement.reset(*m_session);
            statement << "DELETE FROM device WHERE device.key=?;", useRef(deviceKey), now;
            return;
        }

        statement.reset(*m_session);
        statement << "BEGIN TRANSACTION;";

        statement << "DELETE FROM device          WHERE device.key=?;", useRef(deviceKey);
        statement << "DELETE FROM device_manifest WHERE device_manifest.id=?;", useRef(deviceManifestId);

        statement << "COMMIT;", now;
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error removing device with key " << deviceKey;
    }
}

void SQLiteDeviceRepository::removeAll()
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    const auto deviceKeysFromRepository = findAllDeviceKeys();
    for (const std::string& deviceKey : *deviceKeysFromRepository)
    {
        remove(deviceKey);
    }
}

std::unique_ptr<DetailedDevice> SQLiteDeviceRepository::findByDeviceKey(const std::string& deviceKey)
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    Poco::UInt64 deviceManifestId;
    std::string deviceName;

    try
    {
        Statement statement(*m_session);
        statement << "SELECT name, device_manifest_id FROM device WHERE device.key=?;", useRef(deviceKey),
          into(deviceName), into(deviceManifestId);
        if (statement.execute() == 0)
        {
            return nullptr;
        }
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error finding device with key " << deviceKey;
        return nullptr;
    }

    try
    {
        // Device manifest
        std::string manifestName;
        std::string manifestDescription;
        std::string protocol;
        std::string firmwareUpdateProtocol;

        Statement statement(*m_session);
        statement << "SELECT name, description, protocol, firmware_update_protocol FROM device_manifest WHERE id=?;",
          useRef(deviceManifestId), into(manifestName), into(manifestDescription), into(protocol),
          into(firmwareUpdateProtocol), now;

        auto deviceManifest = std::unique_ptr<DeviceManifest>(
          new DeviceManifest(manifestName, manifestDescription, protocol, firmwareUpdateProtocol));

        // Alarm manifests
        std::string alarmReference;
        std::string alarmName;
        std::string alarmSeverityStr;
        std::string alarmMessage;
        std::string alarmDescription;
        statement.reset(*m_session);
        statement
          << "SELECT reference, name, severity, message, description FROM alarm_manifest WHERE device_manifest_id=?;",
          useRef(deviceManifestId), into(alarmReference), into(alarmName), into(alarmSeverityStr), into(alarmMessage),
          into(alarmDescription), range(0, 1);

        while (!statement.done())
        {
            if (statement.execute() == 0)
            {
                break;
            }

            const auto alarmSeverity = [&]() -> AlarmManifest::AlarmSeverity {
                if (alarmSeverityStr == "ALERT")
                {
                    return AlarmManifest::AlarmSeverity::ALERT;
                }
                else if (alarmSeverityStr == "CRITICAL")
                {
                    return AlarmManifest::AlarmSeverity::CRITICAL;
                }
                else if (alarmSeverityStr == "ERROR")
                {
                    return AlarmManifest::AlarmSeverity::ERROR;
                }
                return AlarmManifest::AlarmSeverity::ALERT;
            }();

            deviceManifest->addAlarm(
              AlarmManifest(alarmName, alarmSeverity, alarmReference, alarmMessage, alarmDescription));
        }

        // Actuator manifests
        Poco::UInt64 actuatorManifestId;
        std::string actuatorReference;
        std::string actuatorName;
        std::string actuatorDescription;
        std::string actuatorUnitSymbol;
        std::string actuatorReadingType;
        std::string actuatorDataTypeStr;
        Poco::UInt32 actuatorPrecision;
        double actuatorMinimum;
        double actuatorMaximum;
        std::string actuatorDelimiter;
        statement.reset(*m_session);
        statement << "SELECT id, reference, name, description, unit_symbol, reading_type, data_type, precision, "
                     "minimum, maximum, "
                     "delimiter "
                     "FROM actuator_manifest WHERE device_manifest_id=?;",
          useRef(deviceManifestId), into(actuatorManifestId), into(actuatorReference), into(actuatorName),
          into(actuatorDescription), into(actuatorUnitSymbol), into(actuatorReadingType), into(actuatorDataTypeStr),
          into(actuatorPrecision), into(actuatorMinimum), into(actuatorMaximum), into(actuatorDelimiter), range(0, 1);

        while (!statement.done())
        {
            if (statement.execute() == 0)
            {
                break;
            }

            const auto actuatorDataType = [&]() -> DataType {
                if (actuatorDataTypeStr == "BOOLEAN")
                {
                    return DataType::BOOLEAN;
                }
                else if (actuatorDataTypeStr == "NUMERIC")
                {
                    return DataType::NUMERIC;
                }
                else if (actuatorDataTypeStr == "STRING")
                {
                    return DataType::STRING;
                }

                return DataType::STRING;
            }();

            std::vector<std::string> labels;
            Statement selectLabelsStatement(*m_session);
            selectLabelsStatement << "SELECT label FROM actuator_label WHERE actuator_manifest_id=?;",
              bind(actuatorManifestId), into(labels), now;

            deviceManifest->addActuator(ActuatorManifest(
              actuatorName, actuatorReference, actuatorReadingType, actuatorUnitSymbol, actuatorDataType,
              actuatorPrecision, actuatorDescription, labels, actuatorMinimum, actuatorMaximum));
        }

        // Sensor manifests
        Poco::UInt64 sensorManifestId;
        std::string sensorReference;
        std::string sensorName;
        std::string sensorDescription;
        std::string sensorUnitSymbol;
        std::string sensorReadingType;
        std::string sensorDataTypeStr;
        Poco::UInt32 sensorPrecision;
        double sensorMinimum;
        double sensorMaximum;
        std::string sensorDelimiter;
        statement.reset(*m_session);
        statement << "SELECT id, reference, name, description, unit_symbol, reading_type, data_type, precision, "
                     "minimum, maximum, "
                     "delimiter "
                     "FROM sensor_manifest WHERE device_manifest_id=?;",
          useRef(deviceManifestId), into(sensorManifestId), into(sensorReference), into(sensorName),
          into(sensorDescription), into(sensorUnitSymbol), into(sensorReadingType), into(sensorDataTypeStr),
          into(sensorPrecision), into(sensorMinimum), into(sensorMaximum), into(sensorDelimiter), range(0, 1);

        while (!statement.done())
        {
            if (statement.execute() == 0)
            {
                break;
            }

            const auto sensorDataType = [&]() -> DataType {
                if (sensorDataTypeStr == "BOOLEAN")
                {
                    return DataType::BOOLEAN;
                }
                else if (sensorDataTypeStr == "NUMERIC")
                {
                    return DataType::NUMERIC;
                }
                else if (sensorDataTypeStr == "STRING")
                {
                    return DataType::STRING;
                }

                return DataType::STRING;
            }();

            std::vector<std::string> labels;
            Statement selectLabelsStatement(*m_session);
            selectLabelsStatement << "SELECT label FROM sensor_label WHERE sensor_manifest_id=?;",
              bind(sensorManifestId), into(labels), now;

            deviceManifest->addSensor(SensorManifest(sensorName, sensorReference, sensorReadingType, sensorUnitSymbol,
                                                     sensorDataType, sensorPrecision, sensorDescription, labels,
                                                     sensorMinimum, sensorMaximum));
        }

        // Configuration manifests
        Poco::UInt64 configurationManifestId;
        std::string configurationReference;
        std::string configurationName;
        std::string configurationDescription;
        std::string configurationDataTypeStr;
        double configurationMinimum;
        double configurationMaximum;
        std::string configurationDelimiter;
        std::string configurationDefaultValue;
        statement.reset(*m_session);
        statement << "SELECT id, reference, name, description, data_type, minimum, maximum, delimiter, "
                     "default_value"
                     " FROM configuration_manifest WHERE device_manifest_id=?;",
          useRef(deviceManifestId), into(configurationManifestId), into(configurationReference),
          into(configurationName), into(configurationDescription), into(configurationDataTypeStr),
          into(configurationMinimum), into(configurationMaximum), into(configurationDelimiter),
          into(configurationDefaultValue), range(0, 1);

        while (!statement.done())
        {
            if (statement.execute() == 0)
            {
                break;
            }

            const auto configurationDataType = [&]() -> DataType {
                if (configurationDataTypeStr == "STRING")
                {
                    return DataType::STRING;
                }
                else if (configurationDataTypeStr == "BOOLEAN")
                {
                    return DataType::BOOLEAN;
                }
                else if (configurationDataTypeStr == "NUMERIC")
                {
                    return DataType::NUMERIC;
                }

                return DataType::STRING;
            }();

            std::vector<std::string> labels;
            Statement selectLabelsStatement(*m_session);
            selectLabelsStatement << "SELECT label FROM configuration_label WHERE configuration_manifest_id=?;",
              bind(configurationManifestId), into(labels), now;

            deviceManifest->addConfiguration(ConfigurationManifest(
              configurationName, configurationReference, configurationDataType, configurationDescription,
              configurationDefaultValue, labels, configurationMinimum, configurationMaximum));
        }

        return std::unique_ptr<DetailedDevice>(new DetailedDevice(deviceName, deviceKey, *deviceManifest));
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error deserializing device with key " << deviceKey;
        return nullptr;
    }
}

std::unique_ptr<std::vector<std::string>> SQLiteDeviceRepository::findAllDeviceKeys()
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    auto deviceKeys = std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>());

    try
    {
        Statement statement(*m_session);
        statement << "SELECT key FROM device;", into(*deviceKeys), now;
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error finding device keys";
    }

    return deviceKeys;
}

bool SQLiteDeviceRepository::containsDeviceWithKey(const std::string& deviceKey)
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    try
    {
        Poco::UInt64 deviceCount;
        Statement statement(*m_session);
        statement << "SELECT count(*) FROM device WHERE device.key=?;", useRef(deviceKey), into(deviceCount), now;
        return deviceCount != 0 ? true : false;
    }
    catch (...)
    {
        LOG(ERROR) << "SQLiteDeviceRepository: Error finding device with key " << deviceKey;
        return false;
    }
}

void SQLiteDeviceRepository::update(const DetailedDevice& device)
{
    std::lock_guard<decltype(m_mutex)> l(m_mutex);

    remove(device.getKey());
    save(device);
}

std::string SQLiteDeviceRepository::calculateSha256(const AlarmManifest& alarmManifest)
{
    Poco::Crypto::DigestEngine digestEngine("SHA256");
    digestEngine.update(alarmManifest.getName());
    digestEngine.update(alarmManifest.getReference());
    digestEngine.update(alarmManifest.getMessage());
    digestEngine.update(alarmManifest.getDescription());
    digestEngine.update([&]() -> std::string {
        if (alarmManifest.getSeverity() == wolkabout::AlarmManifest::AlarmSeverity::ALERT)
        {
            return "A";
        }
        else if (alarmManifest.getSeverity() == wolkabout::AlarmManifest::AlarmSeverity::CRITICAL)
        {
            return "C";
        }
        else if (alarmManifest.getSeverity() == wolkabout::AlarmManifest::AlarmSeverity::ERROR)
        {
            return "E";
        }

        poco_assert(false);
        return "";
    }());

    return Poco::Crypto::DigestEngine::digestToHex(digestEngine.digest());
}

std::string SQLiteDeviceRepository::calculateSha256(const ActuatorManifest& actuatorManifest)
{
    Poco::Crypto::DigestEngine digestEngine("SHA256");
    digestEngine.update(actuatorManifest.getName());
    digestEngine.update(actuatorManifest.getReference());
    digestEngine.update(actuatorManifest.getDescription());
    digestEngine.update(actuatorManifest.getUnitSymbol());
    digestEngine.update(actuatorManifest.getReadingTypeName());
    digestEngine.update(std::to_string(actuatorManifest.getPrecision()));
    digestEngine.update(std::to_string(actuatorManifest.getMinimum()));
    digestEngine.update(std::to_string(actuatorManifest.getMaximum()));
    digestEngine.update(actuatorManifest.getDelimiter());

    digestEngine.update([&]() -> std::string {
        if (actuatorManifest.getDataType() == wolkabout::DataType::BOOLEAN)
        {
            return "B";
        }
        else if (actuatorManifest.getDataType() == wolkabout::DataType::NUMERIC)
        {
            return "N";
        }
        else if (actuatorManifest.getDataType() == wolkabout::DataType::STRING)
        {
            return "S";
        }

        poco_assert(false);
        return "";
    }());

    for (const std::string& label : actuatorManifest.getLabels())
    {
        digestEngine.update(label);
    }

    return Poco::Crypto::DigestEngine::digestToHex(digestEngine.digest());
}

std::string SQLiteDeviceRepository::calculateSha256(const SensorManifest& sensorManifest)
{
    Poco::Crypto::DigestEngine digestEngine("SHA256");
    digestEngine.update(sensorManifest.getName());
    digestEngine.update(sensorManifest.getReference());
    digestEngine.update(sensorManifest.getDescription());
    digestEngine.update(sensorManifest.getUnitSymbol());
    digestEngine.update(sensorManifest.getReadingTypeName());
    digestEngine.update(std::to_string(sensorManifest.getPrecision()));
    digestEngine.update(std::to_string(sensorManifest.getMinimum()));
    digestEngine.update(std::to_string(sensorManifest.getMaximum()));
    digestEngine.update(sensorManifest.getDelimiter());

    digestEngine.update([&]() -> std::string {
        if (sensorManifest.getDataType() == wolkabout::DataType::BOOLEAN)
        {
            return "B";
        }
        else if (sensorManifest.getDataType() == wolkabout::DataType::NUMERIC)
        {
            return "N";
        }
        else if (sensorManifest.getDataType() == wolkabout::DataType::STRING)
        {
            return "S";
        }

        poco_assert(false);
        return "";
    }());

    for (const std::string& label : sensorManifest.getLabels())
    {
        digestEngine.update(label);
    }

    return Poco::Crypto::DigestEngine::digestToHex(digestEngine.digest());
}

std::string SQLiteDeviceRepository::calculateSha256(const ConfigurationManifest& configurationManifest)
{
    Poco::Crypto::DigestEngine digestEngine("SHA256");
    digestEngine.update(configurationManifest.getName());
    digestEngine.update(configurationManifest.getReference());
    digestEngine.update(configurationManifest.getDescription());
    digestEngine.update(std::to_string(configurationManifest.getMinimum()));
    digestEngine.update(std::to_string(configurationManifest.getMaximum()));
    digestEngine.update(configurationManifest.getDelimiter());
    digestEngine.update(configurationManifest.getDefaultValue());

    digestEngine.update([&]() -> std::string {
        if (configurationManifest.getDataType() == wolkabout::DataType::BOOLEAN)
        {
            return "B";
        }
        else if (configurationManifest.getDataType() == wolkabout::DataType::NUMERIC)
        {
            return "N";
        }
        else if (configurationManifest.getDataType() == wolkabout::DataType::STRING)
        {
            return "S";
        }

        poco_assert(false);
        return "";
    }());

    for (const std::string& label : configurationManifest.getLabels())
    {
        digestEngine.update(label);
    }

    return Poco::Crypto::DigestEngine::digestToHex(digestEngine.digest());
}

std::string SQLiteDeviceRepository::calculateSha256(const DeviceManifest& deviceManifest)
{
    Poco::Crypto::DigestEngine digestEngine("SHA256");
    digestEngine.update(deviceManifest.getName());
    digestEngine.update(deviceManifest.getDescription());
    digestEngine.update(deviceManifest.getProtocol());
    digestEngine.update(deviceManifest.getFirmwareUpdateType());

    for (const wolkabout::AlarmManifest& alarmManifest : deviceManifest.getAlarms())
    {
        digestEngine.update(calculateSha256(alarmManifest));
    }

    for (const wolkabout::ActuatorManifest& actuatorManifest : deviceManifest.getActuators())
    {
        digestEngine.update(calculateSha256(actuatorManifest));
    }

    for (const wolkabout::SensorManifest& sensorManifest : deviceManifest.getSensors())
    {
        digestEngine.update(calculateSha256(sensorManifest));
    }

    for (const wolkabout::ConfigurationManifest& configurationManifest : deviceManifest.getConfigurations())
    {
        digestEngine.update(calculateSha256(configurationManifest));
    }

    return Poco::Crypto::DigestEngine::digestToHex(digestEngine.digest());
}

}    // namespace wolkabout
