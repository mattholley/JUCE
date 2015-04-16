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

#ifndef JUCE_FILECHANGEWATCHER_H_INCLUDED
#define JUCE_FILECHANGEWATCHER_H_INCLUDED

class JUCE_API  FileChangeWatcher   : public ChangeBroadcaster,
                                      private TimeSliceClient
{
public:

    FileChangeWatcher (const FileFilter* fileFilter,
                           TimeSliceThread& threadToUse);

    /** Destructor. */
    ~FileChangeWatcher();


    const File& getDirectory() const noexcept               { return root; }

    void setDirectory (const File& directory,
                       bool includeDirectories,
                       bool includeFiles);

    void setFileFilter (const FileFilter* newFileFilter);

    const FileFilter* getFilter() const noexcept            { return fileFilter; }

    //==============================================================================
    /** @internal */
    TimeSliceThread& getTimeSliceThread() const noexcept    { return thread; }

private:
    File root;
    const FileFilter* fileFilter;
    TimeSliceThread& thread;
    int fileTypeFlags;

    CriticalSection fileListLock;
    
    ScopedPointer<DirectoryIterator> fileFindHandle;
    bool volatile shouldStop;

    int useTimeSlice() override;
    void changed();
    void setTypeFlags (int);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileChangeWatcher)
};


#endif   // JUCE_FILECHANGEWATCHER_H_INCLUDED
