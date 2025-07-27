#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bitchat
{

// Version negotiation state enum
enum class VersionNegotiationState
{
    NotStarted,
    HelloSent,
    AckReceived,
    Failed
};

// Version hello structure
struct VersionHello
{
    std::vector<uint8_t> supportedVersions;
    uint8_t preferredVersion;
    std::string clientVersion;
    std::string platform;
    std::vector<std::string> capabilities;

    VersionHello() = default;
    VersionHello(const std::vector<uint8_t> &versions, uint8_t preferred, const std::string &version, const std::string &plat)
        : supportedVersions(versions)
        , preferredVersion(preferred)
        , clientVersion(version)
        , platform(plat)
    {
        // Pass
    }
};

// Version ack structure
struct VersionAck
{
    uint8_t agreedVersion;
    std::string serverVersion;
    std::string platform;
    bool rejected;
    std::string reason;

    VersionAck()
        : agreedVersion(0)
        , rejected(false)
    {
        // Pass
    }

    VersionAck(uint8_t version, const std::string &serverVer, const std::string &plat)
        : agreedVersion(version)
        , serverVersion(serverVer)
        , platform(plat)
        , rejected(false)
    {
        // Pass
    }

    VersionAck(uint8_t version, const std::string &serverVer, const std::string &plat, bool reject, const std::string &rejectReason)
        : agreedVersion(version)
        , serverVersion(serverVer)
        , platform(plat)
        , rejected(reject)
        , reason(rejectReason)
    {
        // Pass
    }
};

// Helper class for protocol-related helper functions
class ProtocolHelper
{
public:
    static bool isValidPeerID(const std::string &peerID);
    static bool isValidChannelName(const std::string &channel);
    static bool isValidNickname(const std::string &nickname);

    // Version negotiation helpers
    static uint8_t negotiateVersion(const std::vector<uint8_t> &clientVersions, const std::vector<uint8_t> &serverVersions);
    static std::vector<uint8_t> getSupportedVersions();

private:
    ProtocolHelper() = delete;
};

} // namespace bitchat
