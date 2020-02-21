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

#include <semver200.h>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "sshfs mount";
const std::string fuse_version_string{"FUSE library version"};
const std::string ld_library_path_key{"LD_LIBRARY_PATH="};
const std::string snap_path_key{"SNAP="};

auto run_cmd(mp::SSHSession& session, std::string&& cmd)
{
    auto ssh_process = session.exec(cmd);
    if (ssh_process.exit_code() != 0)
        throw std::runtime_error(ssh_process.read_std_error());

    return ssh_process.read_std_output() + ssh_process.read_std_error();
}

auto get_sshfs_exec_and_options(mp::SSHSession& session)
{
    std::string sshfs_exec;

    try
    {
        // Prefer to use Snap package version first
        std::istringstream sshfs_env{run_cmd(session, "sudo multipass-sshfs.env")};

        auto ld_library_path = mp::utils::match_line_for(sshfs_env, ld_library_path_key);
        auto snap_path = mp::utils::match_line_for(sshfs_env, snap_path_key);
        snap_path = snap_path.substr(snap_path_key.length());

        sshfs_exec = fmt::format("env {} {}/bin/sshfs", ld_library_path, snap_path);
    }
    catch (const std::exception& e)
    {
        mpl::log(mpl::Level::debug, category, fmt::format("'multipass-sshfs' snap package is not installed: {}", e.what()));

        // Fallback to looking for distro version if snap is not found
        try
        {
            sshfs_exec = run_cmd(session, "sudo which sshfs");
        }
        catch (const std::exception& e)
        {
            mpl::log(mpl::Level::warning, category,
                     fmt::format("Unable to determine if 'sshfs' is installed: {}", e.what()));
            throw mp::SSHFSMissingError();
        }
    }

    sshfs_exec = mp::utils::trim_end(sshfs_exec);

    std::istringstream version_info{run_cmd(session, fmt::format("sudo {} -V", sshfs_exec))};

    sshfs_exec += " -o slave -o transform_symlinks -o allow_other";

    auto fuse_version_line = mp::utils::match_line_for(version_info, fuse_version_string);
    if (!fuse_version_line.empty())
    {
        // split on the fuse_version_string along with 0 or more colons
        auto tokens = mp::utils::split(fuse_version_line, fmt::format("{}:* ", fuse_version_string));
        auto fuse_version = tokens[1];

        // The option was removed in libfuse 3.0
        if (!fuse_version.empty() && (version::Semver200_version(fuse_version) < version::Semver200_version("3.0.0")))
            sshfs_exec += " -o nonempty";
    }

    return sshfs_exec;
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

    auto sshfs_exec_line = get_sshfs_exec_and_options(session);

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
                                            default_gid, sshfs_exec_line);
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
