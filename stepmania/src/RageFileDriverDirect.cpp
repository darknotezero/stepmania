#include "global.h"
#include "RageFileDriverDirect.h"
#include "RageUtil.h"
#include "RageUtil_FileDB.h"
#include "RageLog.h"

#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#if !defined(WIN32)
#include <dirent.h>
#include <fcntl.h>
#else
#include <windows.h>
#include <io.h>
#endif

#if !defined(O_BINARY)
#define O_BINARY 0
#endif

/* This driver handles direct file access. */
class DirectFilenameDB: public FilenameDB
{
protected:
	virtual void PopulateFileSet( FileSet &fs, const CString &sPath );
	CString root;

public:
	DirectFilenameDB( CString root_ )
	{
		ExpireSeconds = 30;
		root = root_;
		if( root.Right(1) != "/" )
			root += '/';
		if( root == "./" )
			root = "";
	}
};

void DirectFilenameDB::PopulateFileSet( FileSet &fs, const CString &path )
{
	CString sPath = path;

	/* Resolve path cases (path/Path -> PATH/path). */
	ResolvePath( sPath );

	fs.age.GetDeltaTime(); /* reset */
	fs.files.clear();

#if defined(WIN32)
	WIN32_FIND_DATA fd;

	if ( sPath.size() > 0  && sPath.Right(1) == SLASH )
		sPath.erase( sPath.size() - 1 );

	HANDLE hFind = FindFirstFile( root+sPath+SLASH "*", &fd );

	if( hFind == INVALID_HANDLE_VALUE )
		return;

	do {
		if(!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
			continue;

		File f;
		f.SetName( fd.cFileName );
		f.dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
		f.size = fd.nFileSizeLow;
		f.mtime = fd.ftLastWriteTime.dwLowDateTime;

		fs.files.insert(f);
	} while( FindNextFile( hFind, &fd ) );
	FindClose(hFind);
#else
	int OldDir = open(".", O_RDONLY);
	if( OldDir == -1 )
		RageException::Throw( "Couldn't open(.): %s", strerror(errno) );

	if( chdir(root+sPath) == -1 )
	{
		/* Only log once per dir. */
		if( LOG )
			LOG->MapLog("chdir " + sPath, "Couldn't chdir(%s): %s", sPath.c_str(), strerror(errno) );
		close( OldDir );
		return;
	}
	DIR *d = opendir(".");

	while(struct dirent *ent = readdir(d))
	{
		if(!strcmp(ent->d_name, ".")) continue;
		if(!strcmp(ent->d_name, "..")) continue;
		
		File f;
		f.SetName( ent->d_name );
		
		struct stat st;
		if( stat(ent->d_name, &st) == -1 )
		{
			/* If it's a broken symlink, ignore it.  Otherwise, warn. */
			if( lstat(ent->d_name, &st) == 0 )
				continue;
			
			/* Huh? */
			if(LOG)
				LOG->Warn("Got file '%s' in '%s' from list, but can't stat? (%s)",
					ent->d_name, sPath.c_str(), strerror(errno));
			continue;
		} else {
			f.dir = (st.st_mode & S_IFDIR);
			f.size = st.st_size;
			f.mtime = st.st_mtime;
		}

		fs.files.insert(f);
	}
	       
	closedir(d);
	if( fchdir( OldDir ) == -1 )
		RageException::Throw( "Couldn't fchdir(): %s", strerror(errno) );
	close( OldDir );
#endif
}

class RageFileObjDirect: public RageFileObj
{
private:
	int fd;

public:
	RageFileObjDirect( int fd, RageFile &p );
	virtual ~RageFileObjDirect();
	virtual int Read(void *buffer, size_t bytes);
	virtual int Write(const void *buffer, size_t bytes);
	virtual void Rewind();
	virtual int Seek( int offset );
	virtual int SeekCur( int offset );
	virtual int GetFileSize();
};


RageFileDriverDirect::RageFileDriverDirect( CString root_ ):
	RageFileDriver( new DirectFilenameDB(root_) ),
	root(root_)
{
	if( root.Right(1) != "/" )
		root += '/';
}

/* mkdir -p.  Doesn't fail if Path already exists and is a directory. */
static bool CreateDirectories( CString Path )
{
	CStringArray parts;
	CString curpath;
	split(Path, SLASH, parts);

	for(unsigned i = 0; i < parts.size(); ++i)
	{
		curpath += parts[i] + SLASH;
		if( mkdir(curpath, 0755) == 0 )
			continue;

		if(errno == EEXIST)
			continue;		// we expect to see this error

		// Log the error, but continue on.
		/* When creating a directory that already exists over Samba, Windows is
		 * returning ENOENT instead of EEXIST. */
		/* On Win32 when Path is only a drive letter (e.g. "i:\"), the result is 
		 * EINVAL. */
		if( LOG )
			LOG->Warn("Couldn't create %s: %s", curpath.c_str(), strerror(errno) );

		/* Make sure it's a directory. */
		FlushDirCache();
		if( !IsADirectory(curpath) )
		{
			if( LOG )
				LOG->Warn("Couldn't create %s: path exists and is not a directory", curpath.c_str() );
			
			// HACK: IsADirectory doesn't work if Path contains a drive letter.
			// So, ignore IsADirectory's result and continue trying to create
			// directories anyway.  This shouldn't change behavior, but 
			// is inefficient because we don't bail early on an error.
			//return false;
		}
	}
	
	return true;
}

RageFileObj *RageFileDriverDirect::Open( CString sPath, RageFile::OpenMode mode, RageFile &p, int &err )
{
	sPath = root + sPath;
	/* XXX: make sure this will partially resolve.  eg. if "abc/def" exists,
	 * and we're opening "ABC/DEF/GHI/jkl/mno", make sure this will resolve
	 * to "abc/def/GHI/jkl/mno"; we'll create the missing ones later. */
	FDB->ResolvePath( sPath );

	int flags = O_BINARY;
	if( mode == RageFile::READ )
		flags |= O_RDONLY;
	else
	{
		CString dir = Dirname(sPath);
		if( this->GetFileType(dir) != RageFileManager::TYPE_DIR )
			CreateDirectories( dir );
		flags |= O_WRONLY|O_CREAT|O_TRUNC;
	}

#if defined(XBOX)
	sPath.Replace( "/", "\\" );
#endif

	int fd = open( sPath, flags, 0644 );
	if( fd == -1 )
	{
		err = errno;
		return NULL;
	}
		
	return new RageFileObjDirect( fd, p );
}

#ifdef _WINDOWS
#include "windows.h"
#endif
bool RageFileDriverDirect::Ready()
{
#ifdef _WINDOWS
	// Windows will throw up a message box if we try to write to a
	// removable drive with no disk inserted.  Find out whether there's a 
	// disk in the drive w/o writing a file.

	// find drive letter
	vector<CString> matches;
	static Regex parse("^([A-Za-z]+):");
	parse.Compare( root, matches );
	if( matches.size() != 1 )
		return false;

	CString sDrive = matches[0];
	TCHAR szVolumeNameBuffer[MAX_PATH];
	DWORD dwVolumeSerialNumber;
	DWORD dwMaximumComponentLength;
	DWORD lpFileSystemFlags;
	TCHAR szFileSystemNameBuffer[MAX_PATH];
	BOOL bResult = GetVolumeInformation( 
		sDrive + ":\\",
		szVolumeNameBuffer,
		sizeof(szVolumeNameBuffer),
		&dwVolumeSerialNumber,
		&dwMaximumComponentLength,
		&lpFileSystemFlags,
		szFileSystemNameBuffer,
		sizeof(szFileSystemNameBuffer) );
	return !!bResult;
#else

	// Try to create directory before writing a temp file.
	CreateDirectories( root ); // XXX
	
	// Try to write a file.
	CString sFile = root + "temp"; /* XXX no */
	RageFile f;
	if( !f.Open( sFile, RageFile::WRITE ) )
		return false;

	f.Close();
	remove( sFile );
	return true;
#endif
}


RageFileObjDirect::RageFileObjDirect( int fd_, RageFile &p ):
	RageFileObj( p )
{
	fd = fd_;
	ASSERT( fd != -1 );
}

RageFileObjDirect::~RageFileObjDirect()
{
	if( fd != -1 )
		close( fd );
}

int RageFileObjDirect::Read( void *buf, size_t bytes )
{
	int ret = read( fd, buf, bytes );
	if( ret == -1 )
	{
		SetError( strerror(errno) );
		return -1;
	}

	return ret;
}

int RageFileObjDirect::Write( const void *buf, size_t bytes )
{
	int ret = write( fd, buf, bytes );
	if( ret == -1 )
	{
		SetError( strerror(errno) );
		return -1;
	}

	return ret;
}

void RageFileObjDirect::Rewind()
{
	lseek( fd, 0, SEEK_SET );
}

int RageFileObjDirect::Seek( int offset )
{
	return lseek( fd, offset, SEEK_SET );
}

int RageFileObjDirect::SeekCur( int offset )
{
	return lseek( fd, offset, SEEK_CUR );
}

int RageFileObjDirect::GetFileSize()
{
	const int OldPos = lseek( fd, 0, SEEK_CUR );
	const int ret = lseek( fd, 0, SEEK_END );
	lseek( fd, OldPos, SEEK_SET );
	return ret;
}

/*
 * Copyright (c) 2003 by the person(s) listed below.  All rights reserved.
 *   Glenn Maynard
 *   Chris Danford
 */

