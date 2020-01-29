/*
 * Copyright (C) 2017-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <multipass/sshfs_mount/sshfs_mount.h>

#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/logging/log.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sftp_server.h>
#include <multipass/utils.h>

#include <multipass/format.h>

#include <iostream>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "sshfs mount";
auto run_cmd(mp::SSHSession& session, std::string&& cmd)
{
    auto ssh_process = session.exec(cmd);
    if (ssh_process.exit_code() != 0)
        throw std::runtime_error(ssh_process.read_std_error());

    return ssh_process.read_std_output();
}

mp::SshfsMount::SshfsPkgType get_sshfs_package_type(mp::SSHSession& session)
{
    try
    {
        // Prefer to use snap package version first
        run_cmd(session, "snap list multipass-sshfs");
        return mp::SshfsMount::SshfsPkgType::snap;
    }
    catch (const std::exception& e)
    {
        mpl::log(mpl::Level::debug, category, fmt::format("'sshfs' snap package is not installed: {}", e.what()));
    }

    try
    {
        // Fallback to looking for Debian version if snap is not found
        run_cmd(session, "which sshfs");
        return mp::SshfsMount::SshfsPkgType::debian;
    }
    catch (const std::exception& e)
    {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Unable to determine if 'sshfs' is installed: {}", e.what()));
        throw mp::SSHFSMissingError();
    }
}

void make_target_dir(mp::SSHSession& session, const std::string& target)
{
    run_cmd(session, fmt::format("sudo mkdir -p \"{}\"", target));
}

void set_owner_for(mp::SSHSession& session, const std::string& target)
{
    auto vm_user = run_cmd(session, "id -nu");
    auto vm_group = run_cmd(session, "id -ng");
    mp::utils::trim_end(vm_user);
    mp::utils::trim_end(vm_group);

    run_cmd(session, fmt::format("sudo chown {}:{} \"{}\"", vm_user, vm_group, target));
}

auto make_sftp_server(mp::SSHSession&& session, const std::string& source, const std::string& target,
                      const std::unordered_map<int, int>& gid_map, const std::unordered_map<int, int>& uid_map)
{
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(source = {}, target = {}, …): ", __FILE__, __LINE__, __FUNCTION__, source, target));

    auto sshfs_pkg_type = get_sshfs_package_type(session);

    make_target_dir(session, target);
    set_owner_for(session, target);

    auto output = run_cmd(session, "id -u");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -u` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_uid = std::stoi(output);
    output = run_cmd(session, "id -g");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -g` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_gid = std::stoi(output);

    return std::make_unique<mp::SftpServer>(std::move(session), source, target, gid_map, uid_map, default_uid,
                                            default_gid, sshfs_pkg_type);
}

} // namespace

mp::SshfsMount::SshfsMount(SSHSession&& session, const std::string& source, const std::string& target,
                           const std::unordered_map<int, int>& gid_map, const std::unordered_map<int, int>& uid_map)
    : sftp_server{make_sftp_server(std::move(session), source, target, gid_map, uid_map)}, sftp_thread{[this] {
          std::cout << "Connected" << std::endl;
          sftp_server->run();
          std::cout << "Stopped" << std::endl;
      }}
{
}

mp::SshfsMount::~SshfsMount()
{
    stop();
}

void mp::SshfsMount::stop()
{
    sftp_server->stop();
    if (sftp_thread.joinable())
        sftp_thread.join();
}
