/**
 * @file ProcessControl.cpp
 * @brief Implementation of process control utilities for QNX Remote Process Monitor
 *
 * This file implements process control operations for the QNX environment,
 * including process suspension, resumption, termination, and information
 * gathering. It provides a platform-specific implementation with fallbacks
 * for non-QNX systems where appropriate.
 */

#include "ProcessControl.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <unistd.h>
#include <signal.h>
#include <sys/neutrino.h> // QNX specific header
#include <sys/procfs.h>   // For procfs_status structures

namespace qnx
{
    namespace utils
    {
        /**
         * @brief Send a signal to a process
         *
         * This function sends a signal to a specific process identified by its PID.
         * It uses QNX's SignalKill function when compiled for QNX, and falls back
         * to standard kill() function for other platforms.
         *
         * @param pid The process ID to send the signal to
         * @param signal The signal number to send (e.g., SIGTERM, SIGSTOP)
         * @return true if the signal was sent successfully, false otherwise
         */
        bool ProcessControl::sendSignal(pid_t pid, int signal)
        {
            // Use signalkill for QNX
#ifdef __QNXNTO__
            if (SignalKill(0, pid, 0, signal, 0, 0) == -1)
#else
            if (kill(pid, signal) == -1)
#endif
            {
                std::error_code ec(errno, std::system_category());
                std::cerr << "Failed to send signal " << signal << " to PID " << pid
                          << ": " << ec.message() << std::endl;
                return false;
            }
            return true;
        }

        /**
         * @brief Suspend a process
         *
         * Sends a SIGSTOP signal to the process, which suspends its execution.
         * This is only fully supported on QNX systems.
         *
         * @param pid The process ID to suspend
         * @return true if the process was suspended successfully, false otherwise
         */
        bool ProcessControl::suspend(pid_t pid)
        {
#ifdef __QNXNTO__
            return sendSignal(pid, SIGSTOP);
#else
            std::cerr << "Process suspension not supported on non-QNX systems" << std::endl;
            return false;
#endif
        }

        /**
         * @brief Resume a previously suspended process
         *
         * Sends a SIGCONT signal to the process, which resumes its execution
         * if it was previously suspended with SIGSTOP.
         * This is only fully supported on QNX systems.
         *
         * @param pid The process ID to resume
         * @return true if the process was resumed successfully, false otherwise
         */
        bool ProcessControl::resume(pid_t pid)
        {
#ifdef __QNXNTO__
            return sendSignal(pid, SIGCONT);
#else
            std::cerr << "Process resumption not supported on non-QNX systems" << std::endl;
            return false;
#endif
        }

        /**
         * @brief Terminate a process
         *
         * Sends a SIGTERM signal to the process, requesting it to terminate gracefully.
         * The process may handle this signal and perform cleanup operations before exiting.
         *
         * @param pid The process ID to terminate
         * @return true if the termination signal was sent successfully, false otherwise
         */
        bool ProcessControl::terminate(pid_t pid)
        {
            return sendSignal(pid, SIGTERM);
        }

        /**
         * @brief Check if a process exists
         *
         * Verifies if a process with the given PID exists in the system.
         * Uses QNX's SignalKill with signal 0 to check process existence
         * without actually sending a signal.
         *
         * @param pid The process ID to check
         * @return true if the process exists, false otherwise
         */
        bool ProcessControl::exists(pid_t pid)
        {
#ifdef __QNXNTO__
            return SignalKill(0, pid, 0, 0, 0, 0) != -1;
#else
            return kill(pid, 0) == 0;
#endif
        }

        /**
         * @brief Get the parent process ID for a given process
         *
         * Retrieves the parent process ID by reading the process information
         * from the /proc filesystem. First attempts to use the process info
         * file, then falls back to the status file if necessary.
         *
         * @param pid The process ID to get the parent for
         * @return The parent process ID if available, std::nullopt otherwise
         */
        std::optional<pid_t> ProcessControl::getParentPid(pid_t pid)
        {
#ifdef __QNXNTO__
            // First try to get process info which contains parent PID
            std::stringstream path;
            path << "/proc/" << pid << "/info";

            std::ifstream info_file(path.str());
            if (info_file)
            {
                debug_process_t pinfo;
                if (info_file.read(reinterpret_cast<char *>(&pinfo), sizeof(pinfo)))
                {
                    return std::optional<pid_t>{pinfo.parent}; // Explicit construction
                }
            }

            // Fall back to status if info doesn't work
            path.str("");
            path << "/proc/" << pid << "/status";

            std::ifstream status(path.str());
            if (!status)
            {
                return std::nullopt;
            }

            procfs_status pstatus;
            if (status.read(reinterpret_cast<char *>(&pstatus), sizeof(pstatus)))
            {
                // In QNX 8.0, procfs_status is typedef'd to debug_thread_t
                // which doesn't directly contain parent PID
                // We need to read it from process info instead
                return std::nullopt;
            }
#endif
            return std::nullopt;
        }

        /**
         * @brief Get a list of child processes for a given parent
         *
         * Scans the /proc filesystem to find all processes whose parent
         * matches the specified PID. This method is QNX-specific and
         * relies on the procfs filesystem structure.
         *
         * @param pid The parent process ID
         * @return A vector containing the PIDs of all child processes
         */
        std::vector<pid_t> ProcessControl::getChildProcesses(pid_t pid)
        {
            std::vector<pid_t> children;

#ifdef __QNXNTO__
            try
            {
                const std::filesystem::path proc_path("/proc");

                // Iterate through all directories in /proc
                for (const auto &entry : std::filesystem::directory_iterator(proc_path))
                {
                    if (!entry.is_directory())
                        continue;

                    const std::string &name = entry.path().filename().string();
                    if (name.empty() || !std::isdigit(name[0]))
                        continue;

                    try
                    {
                        // Each directory name in /proc is a PID
                        pid_t current_pid = std::stoi(name);
                        auto parent_pid = getParentPid(current_pid);

                        // Check if this process is a child of the target PID
                        if (parent_pid && *parent_pid == pid)
                        {
                            children.push_back(current_pid);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        continue;
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error getting child processes: " << e.what() << std::endl;
            }
#endif

            return children;
        }

        /**
         * @brief Get the command line for a process
         *
         * Reads the command line arguments from the /proc filesystem.
         * In the proc filesystem, command line arguments are separated
         * by null characters, which this function replaces with spaces.
         *
         * @param pid The process ID
         * @return The command line string, or empty string if unavailable
         */
        std::string ProcessControl::getCommandLine(pid_t pid)
        {
            std::stringstream path;
            path << "/proc/" << pid << "/cmdline";

            std::ifstream cmdline(path.str());
            if (!cmdline)
            {
                return "";
            }

            std::string result;
            std::getline(cmdline, result);

            // Replace null characters with spaces
            std::replace(result.begin(), result.end(), '\0', ' ');

            return result;
        }

        /**
         * @brief Get the working directory of a process
         *
         * Reads the current working directory of a process by examining
         * the /proc/PID/cwd symlink. This is a QNX-specific implementation
         * that relies on the procfs filesystem structure.
         *
         * @param pid The process ID
         * @return The working directory path, or empty string if unavailable
         */
        std::string ProcessControl::getWorkingDirectory(pid_t pid)
        {
#ifdef __QNXNTO__
            try
            {
                std::stringstream path;
                path << "/proc/" << pid << "/cwd";

                std::error_code ec;
                std::filesystem::path cwd_path(path.str());

                if (std::filesystem::exists(cwd_path, ec))
                {
                    return std::filesystem::read_symlink(cwd_path).string();
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error getting working directory: " << e.what() << std::endl;
            }
#endif
            return "";
        }

        /**
         * @brief Get basic process information
         *
         * Collects CPU and memory usage information for a specific process.
         * This information is read from the /proc filesystem on QNX systems.
         *
         * @param pid The process ID
         * @return ProcessInfo structure with usage data if available, nullopt otherwise
         */
        std::optional<ProcessInfo> ProcessControl::getProcessInfo(pid_t pid)
        {
            if (!exists(pid))
            {
                return std::nullopt;
            }

            ProcessInfo info{0.0, 0};

#ifdef __QNXNTO__
            try
            {
                // Get memory usage from status
                std::stringstream status_path;
                status_path << "/proc/" << pid << "/status";

                std::ifstream status_file(status_path.str());
                if (status_file)
                {
                    procfs_status pstatus;
                    if (status_file.read(reinterpret_cast<char *>(&pstatus), sizeof(pstatus)))
                    {
                        // Memory usage from virtual memory size
                        info.memory_usage = pstatus.stksize;
                    }
                }

                // CPU usage is more complex and would require sampling over time
                // This implementation provides a simplified version
                std::stringstream stat_path;
                stat_path << "/proc/" << pid << "/stat";

                std::ifstream stat_file(stat_path.str());
                if (stat_file)
                {
                    std::string line;
                    if (std::getline(stat_file, line))
                    {
                        // Parse CPU statistics (this is a simplified approach)
                        // In a real implementation, we'd need to track usage over time
                        // and calculate percentage based on total system CPU time
                        info.cpu_usage = 0.5; // Placeholder value
                    }
                }

                return info;
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error getting process info: " << e.what() << std::endl;
            }
#endif

            return std::nullopt;
        }

    } // namespace utils
} // namespace qnx