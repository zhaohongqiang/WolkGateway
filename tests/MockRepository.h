#ifndef MOCKREPOSITORY_H
#define MOCKREPOSITORY_H

#include "repository/DeviceRepository.h"
#include <gmock/gmock.h>

class MockRepository : public wolkabout::DeviceRepository
{
public:
    MockRepository() {}
    virtual ~MockRepository() {}

    void save(const wolkabout::Device& device) override{};

    void remove(const std::string& devicekey) override{};

    std::unique_ptr<wolkabout::Device> findByDeviceKey(const std::string& key) override
    {
        return std::unique_ptr<wolkabout::Device>(findByDeviceKeyProxy(key));
    }

    // MOCK_METHOD1(findByDeviceKey, std::unique_ptr<wolkabout::Device>(const std::string& key));

    MOCK_METHOD0(findAllDeviceKeys, std::unique_ptr<std::vector<std::string>>());

    MOCK_METHOD1(containsDeviceWithKey, bool(const std::string& deviceKey));

    MOCK_METHOD1(findByDeviceKeyProxy, wolkabout::Device*(const std::string& key));

private:
    GTEST_DISALLOW_COPY_AND_ASSIGN_(MockRepository);
};

#endif    // MOCKREPOSITORY_H
