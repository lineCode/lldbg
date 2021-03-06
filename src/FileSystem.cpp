#include "FileSystem.hpp"

#include <algorithm>
#include <cstring>

#include "Log.hpp"
#include "fmt/format.h"

std::map<size_t, std::string> FileHandle::s_filepath_cache;
std::map<size_t, std::string> FileHandle::s_filename_cache;
std::map<size_t, std::vector<std::string>> FileHandle::s_contents_cache;
std::mutex FileHandle::s_mutex;

std::optional<FileHandle> FileHandle::create(const std::string& filepath)
{
    const fs::path canonical_path = fs::canonical(filepath);

    // TODO: Log specific reason for any failures
    if (!fs::exists(canonical_path)) {
        return {};
    }

    if (!fs::is_regular_file(canonical_path)) {
        return {};
    }

    if (!canonical_path.has_filename()) {
        return {};
    }

    const size_t path_hash = fs::hash_value(canonical_path);

    std::unique_lock<std::mutex> lock(s_mutex);

    {  // cache the canonical absolute filepath for this file
        auto it = s_filepath_cache.find(path_hash);
        if (it == s_filepath_cache.end()) {
            s_filepath_cache.emplace(path_hash, canonical_path.string());
        }
    }

    {  // cache the short filename for this file: ex (/some/path/foo.txt -> foo.txt)
        auto it = s_filename_cache.find(path_hash);
        if (it == s_filename_cache.end()) {
            s_filename_cache.emplace(path_hash, canonical_path.filename().string());
        }
    }

    return FileHandle(path_hash);
}

const std::vector<std::string>& FileHandle::contents(void)
{
    std::unique_lock<std::mutex> lock(s_mutex);

    if (auto it = s_contents_cache.find(m_hash); it != s_contents_cache.end()) {
        return it->second;
    }

    const std::string& filepath = s_filepath_cache[m_hash];

    std::ifstream infile(filepath);

    std::vector<std::string> contents;

    std::string line;
    while (std::getline(infile, line)) {
        line.shrink_to_fit();
        contents.emplace_back(std::move(line));
    }
    contents.shrink_to_fit();

    LOG(Verbose) << "Read file from disk: " << filepath;

    const auto& [insert_iter, _] = s_contents_cache.emplace(m_hash, std::move(contents));
    return insert_iter->second;
}

const std::string& FileHandle::filepath(void)
{
    std::unique_lock<std::mutex> lock(s_mutex);
    auto it = s_filepath_cache.find(m_hash);
    assert(it != s_filepath_cache.end());
    return it->second;
}

const std::string& FileHandle::filename(void)
{
    std::unique_lock<std::mutex> lock(s_mutex);
    auto it = s_filename_cache.find(m_hash);
    assert(it != s_filename_cache.end());
    return it->second;
}

std::unique_ptr<FileBrowserNode> FileBrowserNode::create(const fs::path& relative_path)
{
    if (!fs::exists(relative_path)) {
        LOG(Error) << "FileBrowser attempted to load non-existent file:" << relative_path;
        return nullptr;
    }

    const fs::path canonical_path = fs::canonical(relative_path);

    if (!fs::is_directory(canonical_path) && !fs::is_regular_file(canonical_path)) {
        LOG(Error) << "Attemped to load a path (" << canonical_path
                   << ") that wasn't a directory or regular file!";
        return nullptr;
    }

    if (!canonical_path.has_filename()) {
        LOG(Error) << "No filename for file: " << canonical_path;
        return nullptr;
    }

    return std::unique_ptr<FileBrowserNode>(new FileBrowserNode(canonical_path));
}

void FileBrowserNode::open_children()
{
    if (fs::is_directory(m_filepath) && !m_opened) {
        for (const fs::path& p : fs::directory_iterator(m_filepath)) {
            std::unique_ptr<FileBrowserNode> new_child_node = FileBrowserNode::create(p);
            if (new_child_node) {
                m_children.emplace_back(std::move(new_child_node));
            }
            else {
                LOG(Warning) << "Encountered invalid object while traversing directory: "
                             << m_filepath;
            }
        }

        std::sort(m_children.begin(), m_children.end(),
                  [](const std::unique_ptr<FileBrowserNode>& a,
                     const std::unique_ptr<FileBrowserNode>& b) {
                      if (a->is_directory() && !b->is_directory()) {
                          return true;
                      }
                      else if (!a->is_directory() && b->is_directory()) {
                          return false;
                      }
                      else {
                          return strcmp(a->filename(), b->filename()) < 0;
                      }
                  });

        m_opened = true;
    }
}

bool OpenFiles::open(const std::string& requested_filepath)
{
    const auto handle_attempt = FileHandle::create(requested_filepath);

    if (handle_attempt) {
        open(*handle_attempt);
        return true;
    }
    else {
        return false;
    }
}

void OpenFiles::open(FileHandle handle)
{
    auto it = std::find(m_files.begin(), m_files.end(), handle);
    if (it != m_files.end()) {
        m_focus = it - m_files.begin();
        LOG(Verbose) << "Successfully opened previously-opened file: " << handle.filepath();
        return;
    }

    m_files.push_back(handle);
    m_focus = m_files.size() - 1;

    LOG(Verbose) << "Successfully opened new file: " << handle.filepath();
    LOG(Verbose) << "Number of currently open files: " << m_files.size();
}

void OpenFiles::close(size_t tab_index)
{
    m_files.erase(m_files.begin() + tab_index);

    if (m_files.empty()) {
        m_focus = {};
    }
    else {
        assert(m_focus);
        const size_t old_open_file_count = m_files.size();
        const size_t old_focused_tab_idx = *m_focus;

        const bool closed_tab_to_left_of_focus = tab_index < old_focused_tab_idx;
        const bool closed_last_tab =
            tab_index == old_focused_tab_idx && old_focused_tab_idx == old_open_file_count - 1;

        if (closed_tab_to_left_of_focus || closed_last_tab) {
            m_focus = old_focused_tab_idx - 1;
        }
    }
}

void BreakPointSet::synchronize(lldb::SBTarget target)
{
    for (auto& [_, bps] : m_cache) bps.clear();

    for (uint32_t i = 0; i < target.GetNumBreakpoints(); i++) {
        lldb::SBBreakpoint bp = target.GetBreakpointAtIndex(i);
        lldb::SBBreakpointLocation location = bp.GetLocationAtIndex(0);

        if (!location.IsValid()) {
            LOG(Error) << "Invalid breakpoint location encountered by LLDB.";
        }

        lldb::SBAddress address = location.GetAddress();

        if (!address.IsValid()) {
            LOG(Error) << "Invalid lldb::SBAddress for breakpoint encountered by LLDB.";
        }

        lldb::SBLineEntry line_entry = address.GetLineEntry();

        const std::string bp_filepath =
            fmt::format("{}/{}", line_entry.GetFileSpec().GetDirectory(),
                        line_entry.GetFileSpec().GetFilename());

        const auto maybe_handle = FileHandle::create(bp_filepath);
        if (!maybe_handle) {
            LOG(Error) << "Invalid filepath found for breakpoint: " << bp_filepath;
            continue;
        }

        const FileHandle handle = *maybe_handle;

        auto it = m_cache.find(handle);

        if (it == m_cache.end()) {
            m_cache.emplace(handle, std::unordered_set<int>({(int)line_entry.GetLine()}));
        }
        else {
            it->second.insert((int)line_entry.GetLine());
        }
    }
}

// void BreakPointSet::add(FileHandle handle, int line)
// {
//     auto it = m_cache.find(handle);
//
//     if (it == m_cache.end()) {
//         m_cache.emplace(handle, {line});
//     }
//     else {
//         it->second.insert(line);
//     }
// }

// void BreakPointSet::remove(FileHandle handle, int line)
// {
//     auto it = m_cache.find(canonical_path);
//
//     if (it == m_cache.end()) {
//         LOG(Error) << "Attempted to remove breakpoint from file with no recorded
//         breakpoints: "
//                    << file;
//     }
//     else {
//         it->second.erase(line);
//     }
// }

const std::unordered_set<int>* BreakPointSet::Get(FileHandle handle)
{
    auto it = m_cache.find(handle);

    if (it == m_cache.end()) {
        return nullptr;
    }
    else {
        return std::addressof(it->second);
    }
}
