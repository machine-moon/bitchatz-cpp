#include "bitchat/helpers/protocol_helper.h"
#include "bitchat/core/constants.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace bitchat
{

bool ProtocolHelper::isValidPeerID(const std::string &peerID)
{
    // Check if the peerID is empty
    if (peerID.empty())
    {
        return false;
    }

    // Must be exactly 16 hex characters (8 bytes)
    if (peerID.length() != 16)
    {
        return false;
    }

    // Check if it contains only hex characters
    // clang-format off
    return std::all_of(peerID.begin(), peerID.end(), [](char c) {
        return std::isxdigit(c);
    });
    // clang-format on
}

bool ProtocolHelper::isValidChannelName(const std::string &channel)
{
    // Check if the channel is empty
    if (channel.empty())
    {
        return false;
    }

    // Check if the channel starts with #
    if (channel[0] != '#')
    {
        return false;
    }

    // Check if the channel is too long (more than 50 characters)
    if (channel.length() > 50)
    {
        return false;
    }

    // Check if it contains only alphanumeric characters, underscores, and hyphens after the #
    // clang-format off
    return std::all_of(channel.begin() + 1, channel.end(), [](char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
    // clang-format on
}

bool ProtocolHelper::isValidNickname(const std::string &nickname)
{
    // Nickname should not be empty and should be reasonable length
    if (nickname.empty() || nickname.length() > 32)
    {
        return false;
    }

    // Check if it contains only alphanumeric characters, underscores, and hyphens
    // clang-format off
    return std::all_of(nickname.begin(), nickname.end(), [](char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
    // clang-format on
}

uint8_t ProtocolHelper::negotiateVersion(const std::vector<uint8_t> &clientVersions, const std::vector<uint8_t> &serverVersions)
{
    // Find the highest common version between client and server
    for (auto it = clientVersions.rbegin(); it != clientVersions.rend(); ++it)
    {
        if (std::find(serverVersions.begin(), serverVersions.end(), *it) != serverVersions.end())
        {
            return *it;
        }
    }

    // No compatible version found
    return 0;
}

std::vector<uint8_t> ProtocolHelper::getSupportedVersions()
{
    // Currently only support version 1
    return {1};
}

} // namespace bitchat
