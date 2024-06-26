#include "vfs.hpp"
#include "../Mem/mm.hpp"
#include "../Tasks/multitasking.hpp"
#include "../Tasks/tty.hpp"

VirtualFilesystem* VirtualFilesystem::active = 0;
VirtualFilesystem::VirtualFilesystem()
{
    num_mounts = 0;
    current_mount = 0;

    /* Descriptors:
     * 0 : stdin
     * 1 : stdout
     * 2 : stderr */

    kernel_file_table.descriptor_index = 4;
    active = this;
}

inline file_entry_t* VirtualFilesystem::file_at(uint32_t index)
{
    file_entry_t* f = ft()->files.at_ptr(index);
    if (f == 0)
        PANIC("VFS failed during 'file_at(index)' out of range");
    return f;
}

file_table* VirtualFilesystem::ft()
{
    if (TM->is_active())
        return TM->file_table();
    return &kernel_file_table;
}

void VirtualFilesystem::mount(Filesystem* fs)
{
    if (num_mounts >= MAX_MOUNTS)
        klog("VFS: Reached max number of mounts");
    mounts[num_mounts] = fs;
    current_mount = num_mounts;
    num_mounts++;
}

int VirtualFilesystem::open_fifo(char* file_name, int flags)
{
    for (int i = 0; i < ft()->files.size(); i++)
        if (strcmp(file_name, file_at(i)->file_name) == 0)
            return file_at(i)->descriptor;

    for (int i = 0; i < kernel_file_table.files.size(); i++) {
        if (strcmp(file_name, kernel_file_table.files.at(i).file_name) == 0) {
            ft()->files.append(kernel_file_table.files.at(i));
            ft()->files.last().descriptor = ft()->descriptor_index;
            ft()->files.last().flags = flags;
            ft()->descriptor_index++;
            return ft()->descriptor_index - 1;
        }
    }

    if ((flags & O_CREAT) == 0)
        return -1;

    if (strncmp(file_name, "/pipe/", 6) != 0)
        return -1;

    file_entry file;
    pipe_t* pipe = Pipe::create();
    strcpy(file.file_name, file_name);
    file.pipe = pipe;
    file.type = FS_TYPE_FIFO;
    file.descriptor = kernel_file_table.descriptor_index;
    kernel_file_table.files.append(file);

    if (kernel_file_table.descriptor_index >= MAX_FILE_DESCRIPTORS) {
        kdbg("VFS: We ran out of kernel descriptors while opening pipe\n");
        kernel_file_table.descriptor_index = 4;
    }
    kernel_file_table.descriptor_index++;

    /* Make sure we get a copy of the pipe file entry */
    return open_fifo(file_name, flags);
}

int VirtualFilesystem::open(char* file_name, int flags)
{
    if (strcmp(file_name, "/dev/klog") == 0)
        return DEV_KLOG_FD;

    if (strcmp(file_name, "/dev/audio") == 0)
        return DEV_AUDIO_FD;

    if (strcmp(file_name, "/dev/mouse") == 0)
        return DEV_MOUSE_FD;

    if (strcmp(file_name, "/dev/keyboard") == 0)
        return DEV_KEYBOARD_FD;

    if (strcmp(file_name, "/dev/display") == 0)
        return DEV_DISPLAY_FD;

    int fifo = open_fifo(file_name, flags & ~(O_CREAT));
    if (fifo != -1)
        return fifo;
    if (strncmp(file_name, "/pipe/", 6) == 0)
        return -1;

    char file_path[MAX_PATH_SIZE];
    memset(file_path, 0, MAX_PATH_SIZE);
    if (file_name[0] != '/')
        TM->task()->cwd(file_path);
    strcat(file_path, file_name);
    path_resolver(file_path, false);

    int mount = -1;
    for (int i = 0; i < num_mounts; i++)
        if (mounts[i]->find_file(file_path) != -1)
            mount = i;

    if (mount == -1) {
        if (flags & O_CREAT) {
            mount = 0; // TODO
        } else {
            return -1;
        }
    }

    file_entry file;
    strcpy(file.file_name, file_path);
    file.mountfs = mount;
    file.descriptor = ft()->descriptor_index;
    /* file.size = mounts[mount]->get_size(file_path); */
    file.type = FS_TYPE_FILE;
    file.flags = flags;
    ft()->files.append(file);

    if (ft()->descriptor_index >= MAX_FILE_DESCRIPTORS) {
        kdbg("VFS: We ran out of descriptors ft=%d\n", (uint32_t)ft());
        kernel_file_table.descriptor_index = 0;
    }

    ft()->descriptor_index++;
    return file.descriptor;
}

/* TODO: We are  not actually closing the fifo here,
 *       we are removing it completely. */
int VirtualFilesystem::close_fifo(int index)
{
    Pipe::destroy(ft()->files[index].pipe);
    for (int i = 0; i < kernel_file_table.files.size(); i++) {
        if (strcmp(kernel_file_table.files[i].file_name, ft()->files[index].file_name) == 0) {
            kernel_file_table.files.remove_at(i);
            return i;
        }
    }

    return -1;
}

int VirtualFilesystem::close(int descriptor)
{
    if (descriptor >= MAX_FILE_DESCRIPTORS)
        return -1;

    int index = search(descriptor);
    if (index == -1)
        return -1;

    if (ft()->files[index].type == FS_TYPE_FIFO) {
        close_fifo(index);
        if (ft() == &kernel_file_table)
            return 0;
    }

    ft()->files.remove_at(index);
    return 0;
}

int VirtualFilesystem::search(int descriptor)
{
    for (int i = 0; i < ft()->files.size(); i++) {
        if (file_at(i)->descriptor == descriptor)
            return i;
    }
    return -1;
}

int VirtualFilesystem::write(int descriptor, uint8_t* data, size_t size)
{
    int index = search(descriptor);
    if ((index == -1) || (size <= 0))
        return -1;

    if (file_at(index)->flags == O_RDONLY)
        return -1;

    if (file_at(index)->type == FS_TYPE_FIFO) {
        if (file_at(index)->flags & O_APPEND)
            return Pipe::append(ft()->files[index].pipe, data, size);
        return Pipe::write(ft()->files[index].pipe, data, size);
    }

    /* TODO: Support O_APPEND in normal files */
    return mounts[ft()->files[index].mountfs]->write_file(ft()->files[index].file_name, data, size);
}

int VirtualFilesystem::read(int descriptor, uint8_t* data, size_t size)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;

    if (file_at(index)->flags & O_WRONLY)
        return -1;

    if (file_at(index)->type == FS_TYPE_FIFO)
        return Pipe::read(file_at(index)->pipe, data, size);

    Filesystem* mount = mounts[file_at(index)->mountfs];
    int read_size = mount->read_file(ft()->files[index].file_name, data, size, ft()->files[index].file_position);
    if (read_size > 0)
        ft()->files[index].file_position += size;
    return read_size;
}

int VirtualFilesystem::stat(int descriptor, struct stat* statbuf)
{
    int index = search(descriptor);
    if (index == -1) {
        statbuf->st_size = -1;
        return -1;
    }

    if (file_at(index)->type == FS_TYPE_FIFO) {
        statbuf->st_size = file_at(index)->pipe->size;
        return 0;
    }

    return mounts[ft()->files[index].mountfs]->stat(ft()->files[index].file_name, statbuf);
}

int VirtualFilesystem::size(int descriptor)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;

    struct stat statbuf;
    if (file_at(index)->type == FS_TYPE_FIFO)
        return file_at(index)->pipe->size;
    mounts[ft()->files[index].mountfs]->stat(ft()->files[index].file_name, &statbuf);
    return statbuf.st_size;
}

bool VirtualFilesystem::is_virtual_directory(char* dirname)
{
    if (strncmp(dirname, "/pipe/", 6) == 0)
        return true;
    return false;
}

int VirtualFilesystem::list_directory(char* dirname, fs_entry_t* entries, uint32_t count)
{
    path_resolver(dirname, true);
    int fscount = mounts[0]->read_dir(dirname, entries, count); // TODO

    if (strncmp(dirname, "/pipe/", 6) == 0) {
        for (uint32_t i = 0; i < kernel_file_table.files.size(); i++) {
            if (kernel_file_table.files.at(i).type == FS_TYPE_FIFO) {
                if (strlen(kernel_file_table.files.at(i).file_name) < 8)
                    continue;
                if (strncmp(kernel_file_table.files.at(i).file_name, "/pipe/", 6) != 0)
                    continue;

                strcpy(entries[fscount].name, kernel_file_table.files.at(i).file_name + 6);
                entries[fscount].type = FS_ENTRY_FIFO;
                fscount++;
            }
        }
    }

    if (strlen(dirname) == 1 && dirname[0] == '/' && fscount < count) {
        strncpy(entries[fscount].name, "pipe", 4);
        entries[fscount].type = FS_ENTRY_VIRTDIR;
        fscount++;
    }

    return fscount;
}

int VirtualFilesystem::unlink(int descriptor)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;
    return mounts[ft()->files[index].mountfs]->unlink(ft()->files[index].file_name);
}
