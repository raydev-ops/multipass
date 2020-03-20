/*
 * Copyright (C) 2017-2019 Canonical, Ltd.
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

#include <QDir>
#include <iostream>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "sshfs mount";
template <typename Callable>
auto run_cmd(mp::SSHSession& session, std::string&& cmd, Callable&& error_handler)
{
    auto ssh_process = session.exec(cmd);
    if (ssh_process.exit_code() != 0)
        error_handler(ssh_process);
    return ssh_process.read_std_output();
}

// Run a command on a given SSH session.
auto run_cmd(mp::SSHSession& session, std::string&& cmd)
{
    auto error_handler = [](mp::SSHProcess& proc) { throw std::runtime_error(proc.read_std_error()); };
    return run_cmd(session, std::forward<std::string>(cmd), error_handler);
}

// Run a command returning a string which can end in space on a given session and reads the result. It adds a
// non-space character (actually, a '-') to signal the end of the resulting string, and later removes the trailing
// spaces and the special character to gather the contents of the return string including trailing spaces of the
// command output.
std::string run_string_cmd(mp::SSHSession& session, std::string&& cmd)
{
    std::string ret;
    ret = run_cmd(session, "echo `" + cmd + "`-");
    mp::utils::trim_end(ret); // Remove the trailing spaces, if any.

    // If the output was indeed correct and it has a '-' at the end, we have to remove it.
    if (ret.size())
    {
        ret.pop_back();
    }

    return ret;
}

// Check if sshfs exists on a given SSH session.
void check_sshfs_exists(mp::SSHSession& session)
{
    auto error_handler = [](mp::SSHProcess& proc) {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Unable to determine if 'sshfs' is installed: {}", proc.read_std_error()));
        throw mp::SSHFSMissingError();
    };

    run_cmd(session, "which sshfs", error_handler);
}

// If the target name starts with ~, this function returns the full directory name. If not, it returns the same
// target name.
std::string expand_home_directory(mp::SSHSession& session, const std::string& target)
{
    std::string expanded_target;

    if ('~' == target[0])
    {
        std::string home;

        // Get the user name, which can be empty.
        auto pos = (1 == target.size()) ? 1 : target.find('/', 1);
        if (1 == pos)
        {
            home = run_string_cmd(session, "pwd");
        }
        else
        {
            std::string username = target.substr(1, pos - 1);
            home = run_string_cmd(session, "getent passwd " + username + " | cut -d : -f 6");
            if (0 == home.size())
            {
                throw std::runtime_error("user " + username + " does not exist or does not have a home defined");
            }
        }

        // Note that target.substr(...) has the directory slash.
        expanded_target = home + target.substr(pos, target.size() - 1);
    }
    else
    {
        expanded_target = target;
    }

    return expanded_target;
}

// Split a path into existing and to-be-created parts.
std::pair<std::string, std::string> get_path_split(mp::SSHSession& session, const std::string& target)
{
    QDir complete_path(QString::fromStdString(target));
    QString absolute;

    if (complete_path.isRelative())
    {
        std::string home = run_string_cmd(session, "pwd");
        absolute = QString::fromStdString(home) + '/' + complete_path.path();
    }
    else
    {
        absolute = complete_path.path();
    }

    QString existing = QString::fromStdString(run_string_cmd(
        session,
        fmt::format("sudo /bin/bash -c 'P=\"{}\"; while [ ! -d \"$P/\" ]; do P=${{P%/*}}; done; echo $P/'", absolute)));

    return {existing.toStdString(), QDir(existing).relativeFilePath(absolute).toStdString()};
}

// Create a directory on a given root folder.
void make_target_dir(mp::SSHSession& session, const std::string& root, const std::string& relative_target)
{
    if (!relative_target.empty())
        run_cmd(session, fmt::format("sudo /bin/bash -c 'cd \"{}\" && mkdir -p \"{}\"'", root, relative_target));
}

// Set ownership of all directories on a path starting on a given root.
// Assume it is already created.
void set_owner_for(mp::SSHSession& session, const std::string& root, const std::string& relative_target)
{
    auto vm_user = run_string_cmd(session, "id -nu");
    auto vm_group = run_string_cmd(session, "id -ng");

    // Get the first directory of the relative path.
    std::string::size_type first_slash = relative_target.find_first_of('/');
    std::string first_dir =
        (std::string::npos == first_slash) ? relative_target : relative_target.substr(0, first_slash);

    run_cmd(session,
            fmt::format("sudo /bin/bash -c 'cd \"{}\" && chown -R {}:{} {}'", root, vm_user, vm_group, first_dir));
}

auto make_sftp_server(mp::SSHSession&& session, const std::string& source, const std::string& target,
                      const std::unordered_map<int, int>& gid_map, const std::unordered_map<int, int>& uid_map)
{
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(source = {}, target = {}, …): ", __FILE__, __LINE__, __FUNCTION__, source, target));

    check_sshfs_exists(session);

    // Expand the ~ if the target contains it.
    std::string expanded_target = expand_home_directory(session, target);

    // Split the path in existing and missing parts.
    const auto& [leading, missing] = get_path_split(session, expanded_target);

    // We need to create the part of the path which does not still exist,
    // and set then the correct ownership.
    make_target_dir(session, leading, missing);
    set_owner_for(session, leading, missing);

    auto output = run_cmd(session, "id -u");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -u` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_uid = std::stoi(output);
    output = run_cmd(session, "id -g");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -g` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_gid = std::stoi(output);

    return std::make_unique<mp::SftpServer>(std::move(session), source, expanded_target, gid_map, uid_map, default_uid,
                                            default_gid);
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
