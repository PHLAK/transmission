// This file Copyright © 2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <array>
#include <chrono>
#include <ctime>
#include <future>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/core.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

#include "libtransmission/transmission.h"

#include "libtransmission/net.h" // tr_address, tr_port

namespace libtransmission
{

class DnsCache
{
public:
    enum class Family
    {
        IPv4,
        IPv6
    };

    enum class Protocol
    {
        TCP,
        UDP
    };

    enum class Result
    {
        Pending,
        Success,
        Failed
    };

    std::tuple<Result, sockaddr_storage, socklen_t> get(
        std::string_view host,
        tr_port port,
        time_t now,
        Family family,
        Protocol protocol) noexcept
    {
        auto const key = Key{ host, port, family, protocol };

        if (auto const iter = pending_.find(key); iter != std::end(pending_))
        {
            if (auto& fut = iter->second; fut.wait_for(std::chrono::milliseconds{ 0 }) == std::future_status::ready)
            {
                auto const addr = fut.get();
                cache_[key] = addr ? Cache{ *addr, now, Result::Success } : Cache{ {}, now, Result::Failed };
                pending_.erase(iter);
            }
            else
            {
                return { Result::Pending, {}, {} };
            }
        }

        if (auto const iter = cache_.find(key); iter != std::end(cache_))
        {
            if (auto const& [addr, created_at, result] = iter->second; now - created_at < CacheTtlSecs)
            {
                return { result, addr.first, addr.second };
            }

            cache_.erase(iter); // expired
        }

        pending_[key] = std::async(std::launch::async, lookup, std::string{ host }, port, family, protocol);
        return { Result::Pending, {}, {} };
    }

    [[nodiscard]] bool is_pending(std::string_view host, tr_port port, Family family, Protocol protocol) const noexcept
    {
        auto const key = Key{ host, port, family, protocol };
        return pending_.count(key) != 0U;
    }

    [[nodiscard]] auto dump(
        time_t now, //
        std::optional<Family> family_wanted = {}, //
        std::optional<Protocol> protocol_wanted = {}) const
    {
        check_pending(now);

        auto tmp = std::map<std::string /*host:port*/, std::string /*comma-separated addresses*/>{};

        for (auto const& [key, cache] : cache_)
        {
            if (cache.result != Result::Success)
            {
                continue;
            }

            auto const& [host, port, family, protocol] = key;

            if (family_wanted && *family_wanted != family)
            {
                continue;
            }

            if (protocol_wanted && *protocol_wanted != protocol)
            {
                continue;
            }

            if (auto addrport = tr_address::from_sockaddr(reinterpret_cast<sockaddr const*>(&cache.addr)); addrport)
            {
                auto& addresses = tmp[fmt::format("{:s}:{:d}", host, port.host())];
                if (!std::empty(addresses))
                {
                    addresses += ',';
                }
                addresses += addrport->first.display_name();
            }
        }

        auto ret = std::vector<std::string>{};
        ret.reserve(std::size(tmp));
        for (auto const& [hostport, addresses] : tmp)
        {
            ret.emplace_back(fmt::format("{:s}:{:s}", hostport, addresses));
        }
        return ret;
    }

private:
    using Sockaddr = std::pair<sockaddr_storage, socklen_t>;
    using Key = std::tuple<std::string, tr_port, Family, Protocol>;
    using MaybeSockaddr = std::optional<Sockaddr>;

    struct Cache
    {
        Sockaddr addr = {};
        time_t created_at = {};
        Result result = Result::Failed;
    };

    [[nodiscard]] static MaybeSockaddr lookup(std::string host, tr_port port, Family family, Protocol protocol)
    {
        auto szport = std::array<char, 16>{};
        *fmt::format_to(std::data(szport), FMT_STRING("{:d}"), port.host()) = '\0';

        auto hints = addrinfo{};
        hints.ai_family = family == Family::IPv4 ? AF_INET : AF_INET6;
        hints.ai_protocol = protocol == Protocol::TCP ? IPPROTO_TCP : IPPROTO_UDP;
        hints.ai_socktype = protocol == Protocol::TCP ? SOCK_STREAM : SOCK_DGRAM;

        addrinfo* info = nullptr;
        if (int const rc = getaddrinfo(host.c_str(), std::data(szport), &hints, &info); rc != 0)
        {
            return {};
        }

        auto ss = sockaddr_storage{};
        auto const len = info->ai_addrlen;
        memcpy(&ss, info->ai_addr, len);
        freeaddrinfo(info);

        return std::make_pair(ss, len);
    }

    void check_pending(time_t now) const
    {
        if (auto iter = std::begin(pending_); iter != std::end(pending_))
        {
            if (auto& [key, fut] = *iter; fut.wait_for(std::chrono::milliseconds{ 0 }) == std::future_status::ready)
            {
                auto const addr = fut.get();
                cache_[key] = addr ? Cache{ *addr, now, Result::Success } : Cache{ {}, now, Result::Failed };
                iter = pending_.erase(iter);
            }
        }
    }

    static inline constexpr auto CacheTtlSecs = time_t{ 3600 };

    mutable std::map<Key, Cache> cache_;
    mutable std::map<Key, std::future<MaybeSockaddr>> pending_;
};

} // namespace libtransmission