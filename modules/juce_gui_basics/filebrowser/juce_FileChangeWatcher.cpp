/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2013 - Raw Material Software Ltd.

   Permission is granted to use this software under the terms of either:
   a) the GPL v2 (or any later version)
   b) the Affero GPL v3

   Details of these licenses can be found at: www.gnu.org/licenses

   JUCE is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
   A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

   ------------------------------------------------------------------------------

   To release a closed-source product which uses JUCE, commercial licenses are
   available: visit www.juce.com for more information.

  ==============================================================================
*/

FileChangeWatcher::FileChangeWatcher (const FileFilter* f, TimeSliceThread& t)
   : fileFilter (f), thread (t),
     fileTypeFlags (File::ignoreHiddenFiles | File::findFiles),
     shouldStop (true)
{
    thread.addTimeSliceClient (this);
}

FileChangeWatcher::~FileChangeWatcher()
{
}

//==============================================================================
void FileChangeWatcher::setDirectory (const File& directory,
                                          const bool includeDirectories,
                                          const bool includeFiles)
{
    jassert (includeDirectories || includeFiles); // you have to speciify at least one of these!

    if (directory != root)
    {
        root = directory;
        changed();

        // (this forces a refresh when setTypeFlags() is called, rather than triggering two refreshes)
        fileTypeFlags &= ~(File::findDirectories | File::findFiles);
    }

    int newFlags = fileTypeFlags;
    if (includeDirectories) newFlags |= File::findDirectories;  else newFlags &= ~File::findDirectories;
    if (includeFiles)       newFlags |= File::findFiles;        else newFlags &= ~File::findFiles;

    setTypeFlags (newFlags);
}

void FileChangeWatcher::setTypeFlags (const int newFlags)
{
    if (fileTypeFlags != newFlags)
    {
        fileTypeFlags = newFlags;
    }
}

void FileChangeWatcher::setFileFilter (const FileFilter* newFileFilter)
{
    const ScopedLock sl (fileListLock);
    fileFilter = newFileFilter;
}

//==============================================================================

void FileChangeWatcher::changed()
{
    sendChangeMessage();
}

//==============================================================================
int FileChangeWatcher::useTimeSlice()
{
    const uint32 startTime = Time::getApproximateMillisecondCounter();

    HANDLE hDirectory = FindFirstChangeNotification    (root.getFullPathName().toWideCharPointer(),
                                                        TRUE,
										                FILE_NOTIFY_CHANGE_FILE_NAME  |
										                FILE_NOTIFY_CHANGE_DIR_NAME   |
										                FILE_NOTIFY_CHANGE_SIZE       |
										                FILE_NOTIFY_CHANGE_LAST_WRITE |
										                FILE_NOTIFY_CHANGE_ATTRIBUTES);

    if (hDirectory == INVALID_HANDLE_VALUE)
    {
        return 500;
    }

    for (int i = 1; --i >= 0;)
    {
        if (WaitForSingleObject (hDirectory, 10) == WAIT_OBJECT_0)
        {
            changed();

            return 500;
        }

        if (shouldStop || (Time::getApproximateMillisecondCounter() > startTime + 150))
            break;

        if (FindNextChangeNotification (hDirectory) == 0)
            break;
    }

    FindCloseChangeNotification (hDirectory);

    return 0;
}
