#include "vfs.hpp"

VirtualFilesystem* VirtualFilesystem::active = 0;
VirtualFilesystem::VirtualFilesystem()
{
    num_mounts = 0;
    current_mount = 0;
    num_open_files = 0;

    /* Descriptors:
     * 0 : stdin
     * 1 : stdout
     * 2 : stderr */

    file_descriptors = 4;
    active = this;
}

void VirtualFilesystem::mount(Filesystem* fs)
{
    mounts[num_mounts] = fs;
    current_mount = num_mounts;
    num_mounts++;
}

int VirtualFilesystem::open_pipe(char* file_name, int flags)
{
    for (int i = 0; i < num_open_files; i++)
        if (strcmp(file_name, files[i].file_name) == 0)
            return files[i].descriptor;

    if (flags == 0)
        return -1;

    file_entry file;
    pipe_t pipe = Pipe::create();
    strcpy(file.file_name, file_name);
    file.pipe = pipe;
    file.type = FS_PIPE;
    file.descriptor = file_descriptors;
    files[num_open_files] = file;

    if (file_descriptors >= MAX_FILE_DESCRIPTORS)
        file_descriptors = 0;

    num_open_files++;
    file_descriptors++;
    return file.descriptor;
}

int VirtualFilesystem::open(char* file_name, int type, int flags)
{
    if (strcmp(file_name, "/dev/audio") == 0)
        return DEV_AUDIO_FD;

    if (type == FS_PIPE)
        return open_pipe(file_name, flags);

    char file_path[MAX_PATH_SIZE];
    memset(file_path, 0, MAX_PATH_SIZE);
    TM->task()->cwd(file_path);
    strcat(file_path, file_name);
    path_resolver(file_path, false);

    int mount = -1;
    for (int i = 0; i < num_mounts; i++)
        if (mounts[i]->find_file(file_path) != -1)
            mount = i;

    if (mount == -1)
        return -1;

    file_entry file;
    strcpy(file.file_name, file_path);
    file.mountfs = mount;
    file.descriptor = file_descriptors;
    file.size = mounts[mount]->get_size(file_path);
    file.type = type;
    files[num_open_files] = file;

    if (file_descriptors >= MAX_FILE_DESCRIPTORS)
        file_descriptors = 0;

    num_open_files++;
    file_descriptors++;

    return file.descriptor;
}

int VirtualFilesystem::close(int descriptor)
{
    if (descriptor >= MAX_FILE_DESCRIPTORS)
        return 1;

    int index = search(descriptor);
    if (index == -1)
        return -1;

    for (int j = index; j < num_open_files - 1; j++)
        files[j] = files[j + 1];

    num_open_files--;
    return 0;
}

int VirtualFilesystem::search(int descriptor)
{
    for (int i = 0; i < num_open_files; i++) {
        if (files[i].descriptor == descriptor)
            return i;
    }
    return -1;
}

int VirtualFilesystem::write(int descriptor, uint8_t* data, int size)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;

    if (files[index].type == FS_PIPE)
        return Pipe::write(files[index].pipe, data, size);
    return mounts[files[index].mountfs]->write_file(files[index].file_name, data, size);
}

int VirtualFilesystem::read(int descriptor, uint8_t* data, int size)
{
    int index = search(descriptor);
    if (index == -1)
        return 0;

    if (files[index].type == FS_PIPE)
        return Pipe::read(files[index].pipe, data, files[index].pipe.size);

    int read_size = mounts[files[index].mountfs]->read_file(files[index].file_name, data, size, files[index].file_position);
    if ((read_size != -1) && (read_size))
        files[index].file_position += size;
    if (read_size < 0)
        read_size = 0;
    return read_size;
}

int VirtualFilesystem::size(int descriptor)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;

    if (files[index].type == FS_PIPE)
        return files[index].pipe.size;
    return mounts[files[index].mountfs]->get_size(files[index].file_name);
}

int VirtualFilesystem::uid(int descriptor)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;
    return mounts[files[index].mountfs]->get_uid(files[index].file_name);
}

int VirtualFilesystem::gid(int descriptor)
{
    int index = search(descriptor);
    if (index == -1)
        return -1;
    return mounts[files[index].mountfs]->get_gid(files[index].file_name);
}

int VirtualFilesystem::listdir(char* dirname, char** entries)
{
    path_resolver(dirname);

    int exists = -1;
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i]->read_dir(dirname, entries) == 0)
            exists = 0;
    }
    return exists;
}
