#include "btfiles.h"

#ifdef WINDOWS
#include <io.h>
#include <memory.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/param.h>
#endif

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>  // isprint

#include "btconfig.h"
#include "bencode.h"
#include "btcontent.h"
#include "console.h"
#include "bttime.h"

#if !defined(HAVE_SNPRINTF) || !defined(HAVE_FSEEKO)
#include "compat.h"
#endif

#define MAX_OPEN_FILES 20                // max simultaneous open data files
#define OPT_IO_SIZE (256*1024)           // optimal I/O size for large ops
#define MAX_STAGEFILE_SIZE (2*1024*1024) // [soft] size limit of a staging file
#define MAX_STAGEDIR_FILES 200           // max staging files per directory
#define WRITE_RETRY_INTERVAL 300     // seconds to retry after disk write error

btFiles::btFiles()
{
  m_btfhead = (BTFILE *)0;
  m_nfiles = 0;
  m_file = (BTFILE **)0;
  m_total_files_length = 0;
  m_total_opened = 0;
  m_flag_automanage = 1;
  m_need_merge = 0;
  m_directory = (char *)0;
  m_staging_path = m_stagedir = (char *)0;
  m_stagecount = 0;
  m_write_failed = false;
  m_write_tried = (time_t)0;
}

btFiles::~btFiles()
{
  struct stat sb;
  DIR *dp;
  struct dirent *dirp;
  int f_remove = 1;

  _btf_destroy();

  if( !g_secondary_process &&
      0==stat(m_staging_path, &sb) && S_ISDIR(sb.st_mode) &&
      (dp = opendir(m_staging_path)) ){
    while( (dirp = readdir(dp)) ){
      if( 0!=strcmp(dirp->d_name, ".") && 0!=strcmp(dirp->d_name, "..") ){
        f_remove = 0;
        break;
      }
    }
    closedir(dp);
    if( f_remove ){
      if(*cfg_verbose) CONSOLE.Debug("Remove dir \"%s\"", m_staging_path);
      if( remove(m_staging_path) < 0 )
        CONSOLE.Warning(2, "warn, remove directory \"%s\" failed:  %s",
          m_staging_path, strerror(errno));
    }
  }
  if( m_directory ) delete []m_directory;
  if( m_file ) delete []m_file;
  if( m_staging_path ) delete []m_staging_path;
  if( m_stagedir ) delete []m_stagedir;
}

void btFiles::CloseFile(dt_count_t nfile)
{
  if( nfile && nfile <= m_nfiles )
    _btf_close(m_file[nfile-1]);
}

int btFiles::_btf_close_oldest()
{
  BTFILE *pbf_n, *pbf_close;
  pbf_close = (BTFILE *)0;
  for( pbf_n = m_btfhead; pbf_n; pbf_n = pbf_n->bf_next ){
    if( !pbf_n->bf_flag_opened ) continue;
    if( !pbf_close || pbf_n->bf_last_timestamp < pbf_close->bf_last_timestamp )
      pbf_close = pbf_n;
  }
  if( !pbf_close ){
    errno = ENOENT;
    return -1;
  }
  return _btf_close(pbf_close);
}

int btFiles::_btf_close(BTFILE *pbf)
{
  if( !pbf->bf_flag_opened ) return 0;

  if(*cfg_verbose) CONSOLE.Debug("Close file \"%s\"", pbf->bf_filename);

  if( fclose(pbf->bf_fp) == EOF )
    CONSOLE.Warning(2, "warn, error closing file \"%s\":  %s",
      pbf->bf_filename, strerror(errno));
  pbf->bf_flag_opened = 0;
  pbf->bf_fp = (FILE *)0;
  if( pbf->bf_buffer ){
    delete []pbf->bf_buffer;
    pbf->bf_buffer = (char *)0;
  }
  m_total_opened--;
  return 0;
}

int btFiles::_btf_open(BTFILE *pbf, const int iotype)
{
  char fn[MAXPATHLEN];
  const char *mode = iotype ? (pbf->bf_size ? "r+b" : "w+b") : "rb";
  struct stat sb;

  if( pbf->bf_flag_opened ){
    if( pbf->bf_flag_readonly && iotype ) _btf_close(pbf);
    else return 0;  // already open in a usable mode
  }

  if( m_flag_automanage && m_total_opened >= MAX_OPEN_FILES ){  // close a file
    if( _btf_close_oldest() < 0 ) return -1;
  }

  if(*cfg_verbose) CONSOLE.Debug("Open mode=%s %sfile \"%s\"", mode,
    pbf->bf_flag_staging ? "staging " : "", pbf->bf_filename);

  if( pbf->bf_flag_staging ){
    if( MAXPATHLEN <= snprintf(fn, MAXPATHLEN, "%s%c%s", m_staging_path,
                      PATH_SP, pbf->bf_filename) ){
      errno = ENAMETOOLONG;
      return -1;
    }
  }else if( m_directory ){
    if( MAXPATHLEN <= snprintf(fn, MAXPATHLEN, "%s%c%s", m_directory, PATH_SP,
                               pbf->bf_filename) ){
      errno = ENAMETOOLONG;
      return -1;
    }
  }else{
    strcpy(fn, pbf->bf_filename);
  }

  if( iotype && stat(fn, &sb) < 0 && MkPath(fn) < 0 ){
    CONSOLE.Warning(1,
      "error, create directory path for file \"%s\" failed:  %s",
      strerror(errno));
    return -1;
  }

  pbf->bf_last_timestamp = now + 1;
  if( !(pbf->bf_fp = fopen(fn, mode)) ){
    switch( errno ){
    case EMFILE:
    case ENFILE:
      _btf_close_oldest();
      break;
    case ENOSPC:
      if( !MergeNext() ) MergeAny();  // directory could be full
      break;
    default:
      return -1;
    }
    if( !(pbf->bf_fp = fopen(fn, mode)) ) return -1;  // caller prints error
  }
  pbf->bf_buffer = new char[DEFAULT_SLICE_SIZE];
  if( pbf->bf_buffer )
    setvbuf(pbf->bf_fp, pbf->bf_buffer, _IOFBF, DEFAULT_SLICE_SIZE);

  pbf->bf_flag_opened = 1;
  pbf->bf_flag_readonly = iotype ? 0 : 1;
  m_total_opened++;
  return 0;
}

int btFiles::IO(char *rbuf, const char *wbuf, dt_datalen_t off, bt_length_t len)
{
  int result = -1;
  const int iotype = wbuf ? 1 : 0;
  off_t pos;
  size_t nio;
  BTFILE *pbf = m_btfhead, *pbfref = (BTFILE *)0, *pbfnext = (BTFILE *)0;
  bool diskaccess = false;

  if( off + (dt_datalen_t)len > m_total_files_length ){
    CONSOLE.Warning(1, "error, data offset %llu length %lu out of range",
      (unsigned long long)off, (unsigned long)len);
    errno = EINVAL;
    return -1;
  }

  // Break up the I/O if necessary due to system limitation.
  if( len > (size_t)len ){
    bt_length_t iosize;
    int r;

    result = 0;
    for( iosize = len; iosize > (size_t)iosize; iosize /= 2 );
    while( len ){
      if( len < iosize ) iosize = len;
      r = IO(rbuf, wbuf, off, iosize);
      if( r != 0 ) result = r;
      if( iotype ) wbuf += iosize;
      else rbuf += iosize;
      len -= iosize;
    }
    return result;
  }

  // Find the first file to read/write
  while( pbf ){
    pbfnext = pbf->bf_next;
    if( off >= pbf->bf_offset &&
        (off < pbf->bf_offset + pbf->bf_size ||
          (iotype && off == pbf->bf_offset + pbf->bf_size &&
            (!pbf->bf_flag_staging || pbf->bf_size < MAX_STAGEFILE_SIZE))) ){
      break;
    }
    if( off < pbf->bf_offset ){
      pbf = (BTFILE *)0;
      break;
    }
    pbfref = pbf;
    pbf = (!pbf->bf_flag_staging && off >= pbf->bf_offset + pbf->bf_length) ?
          pbf->bf_nextreal : pbf->bf_next;
  }

  // Read/write the data (all applicable files)
  while( len ){
    if( iotype && pbf && pbf->bf_flag_staging &&
        pbf->bf_size >= MAX_STAGEFILE_SIZE &&
        off == pbf->bf_offset + pbf->bf_size ){
      pbfref = pbf;
      pbf = (BTFILE *)0;
    }
    if( !pbf ){
      if( iotype ){  // write
        // Create new staging file
        if( m_stagecount >= MAX_STAGEDIR_FILES || !m_stagedir[0] ){
          char fn[MAXPATHLEN], tmpdir[m_fsizelen+1];
          sprintf(tmpdir, "%.*llu", (int)m_fsizelen, (unsigned long long)off);
          snprintf(fn, MAXPATHLEN, "%s%c%s", m_staging_path, PATH_SP, tmpdir);
          if(*cfg_verbose) CONSOLE.Debug("Create dir \"%s\"", fn);
          if( mkdir(fn, 0755) < 0 ){
            CONSOLE.Warning(1, "error, create directory \"%s\" failed:  %s",
              fn, strerror(errno));
          }else{
            strcpy(m_stagedir, tmpdir);
            m_stagecount = 0;
          }
          diskaccess = true;
        }
        if( !(pbf = new BTFILE) ||
            !(pbf->bf_filename = new char[strlen(m_stagedir) +
              strlen(m_torrent_id) + m_fsizelen + 3]) ){
          CONSOLE.Warning(1,
            "error, failed to allocate memory for staging file");
          if( pbf ) delete pbf;
          errno = ENOMEM;
          goto done;
        }
        sprintf(pbf->bf_filename, "%s%c%s-%.*llu", m_stagedir, PATH_SP,
          m_torrent_id, (int)m_fsizelen, (unsigned long long)off);
        pbf->bf_offset = off;
        pbf->bf_flag_staging = 1;
        pbf->bf_next = pbfref->bf_next;
        pbf->bf_nextreal = pbfref->bf_nextreal;
        pbfref->bf_next = pbf;
        m_stagecount++;
      }else{  // read
        CONSOLE.Warning(1, "error, failed to find file for offset %llu",
          (unsigned long long)off);
        errno = EINVAL;
        goto done;
      }
    }

    pos = off - pbf->bf_offset;

    if( (!pbf->bf_flag_opened || (iotype && pbf->bf_flag_readonly)) &&
        _btf_open(pbf, iotype) < 0 ){
      CONSOLE.Warning(1, "error, failed to open file \"%s\":  %s",
        pbf->bf_filename, strerror(errno));
      diskaccess = true;
      goto done;
    }

    pbf->bf_last_timestamp = now;

    diskaccess = true;
    if( fseeko(pbf->bf_fp, pos, SEEK_SET) < 0 ){
      CONSOLE.Warning(1, "error, failed to seek to %llu on file \"%s\":  %s",
        (unsigned long long)pos, pbf->bf_filename, strerror(errno));
      goto done;
    }

    // Read or write current file
    if( 0 == iotype ){
      nio = (len <= pbf->bf_size - pos) ? len : (pbf->bf_size - pos);
      errno = 0;
      if( nio && 1 != fread(rbuf, nio, 1, pbf->bf_fp) && ferror(pbf->bf_fp) ){
        CONSOLE.Warning(1, "error, read failed at %llu on file \"%s\":  %s",
          (unsigned long long)pos, pbf->bf_filename, strerror(errno));
        goto done;
      }
    }else{
      if( pbf->bf_flag_staging ){
        if( !pbf->bf_next ||
            len <= pbf->bf_next->bf_offset - pbf->bf_offset - pos ){
          nio = len;
        }else nio = pbf->bf_next->bf_offset - pbf->bf_offset - pos;
      }else{
        nio = (len <= pbf->bf_length - pos) ? len : (pbf->bf_length - pos);
      }
      errno = 0;
      if( nio ){
        if( 1 != fwrite(wbuf, nio, 1, pbf->bf_fp) ||
            fflush(pbf->bf_fp) == EOF ){
          CONSOLE.Warning(1,
            "error, write or flush failed at %llu on file \"%s\":  %s",
            (unsigned long long)pos, pbf->bf_filename, strerror(errno));
          m_write_failed = true;
          m_write_tried = now;
          goto done;
        }
        m_write_failed = false;
      }
      if( (dt_datalen_t)pos + nio > pbf->bf_size )
        pbf->bf_size = pos + nio;
      if( !pbf->bf_flag_staging && pbf->bf_size < pbf->bf_length &&
          pbf->bf_next && pbf->bf_next->bf_flag_staging &&
          pbf->bf_offset + pbf->bf_size >= pbf->bf_next->bf_offset ){
        m_need_merge = 1;
      }
      if( pbf->bf_size == 0 ) _btf_close(pbf);
    }

    // Proceed to next file
    len -= nio;
    if( len ){
      off += nio;
      if( iotype ) wbuf += nio;
      else rbuf += nio;
      pbfref = pbf;
      pbf = pbf->bf_next;
      if( off < pbf->bf_offset ){
        pbfnext = pbf;
        pbf = (BTFILE *)0;
      }
    }
  }
  result = 0;
 done:
  if( diskaccess ) DiskAccess();
  return result;
}

int btFiles::NeedMerge() const
{
  if( !m_need_merge ) return 0;
  return ( m_write_failed && now < m_write_tried + WRITE_RETRY_INTERVAL ) ?
    0 : 1;
}

int btFiles::MergeStaging(BTFILE *dst)
{
  int result = -1;
  BTFILE *src = dst->bf_next;
  char buf[OPT_IO_SIZE];
  size_t nio = OPT_IO_SIZE;
  off_t pos;
  dt_datalen_t remain;
  int f_remove = 0;
  bool diskaccess = false;

  if( src->bf_offset + src->bf_size <= dst->bf_offset + dst->bf_size ){
    if(*cfg_verbose)
      CONSOLE.Debug("Staging file %s range already present in \"%s\"",
        src->bf_filename, dst->bf_filename);
    goto delsrc;
  }
  if(*cfg_verbose) CONSOLE.Debug("Merge file %s to \"%s\"", src->bf_filename,
    dst->bf_filename);

  if( !src->bf_flag_opened && _btf_open(src, 0) < 0 ){
    CONSOLE.Warning(1, "error, failed to open file \"%s\":  %s",
      src->bf_filename, strerror(errno));
    diskaccess = true;
    goto done;
  }
  pos = dst->bf_offset + dst->bf_size - src->bf_offset;
  remain = src->bf_size - pos;
  diskaccess = true;
  if( fseeko(src->bf_fp, pos, SEEK_SET) < 0 ){
    CONSOLE.Warning(1, "error, failed to seek to %llu on file \"%s\":  %s",
      (unsigned long long)pos, src->bf_filename, strerror(errno));
    goto done;
  }

  // Prevent src from being closed during open of dst.
  src->bf_last_timestamp = now + 1;

  if( (!dst->bf_flag_opened || dst->bf_flag_readonly) &&
      _btf_open(dst, 1) < 0 ){
    CONSOLE.Warning(1, "error, failed to open file \"%s\":  %s",
      dst->bf_filename, strerror(errno));
    goto done;
  }
  pos = dst->bf_size;
  if( fseeko(dst->bf_fp, pos, SEEK_SET) < 0 ){
    CONSOLE.Warning(1, "error, failed to seek to %llu on file \"%s\":  %s",
      (unsigned long long)pos, dst->bf_filename, strerror(errno));
    goto done;
  }

  while( remain && dst->bf_size < dst->bf_length ){
    if( remain < nio ) nio = remain;
    errno = 0;
    if( 1 != fread(buf, nio, 1, src->bf_fp) && ferror(src->bf_fp) ){
      CONSOLE.Warning(1, "error, read failed at %llu on file \"%s\":  %s",
        (unsigned long long)(src->bf_size - remain), src->bf_filename,
        strerror(errno));
      goto done;
    }
    if( 1 != fwrite(buf, nio, 1, dst->bf_fp) || fflush(dst->bf_fp) == EOF ){
      CONSOLE.Warning(1,
        "error, write or flush failed at %llu on file \"%s\":  %s",
        (unsigned long long)dst->bf_size, dst->bf_filename, strerror(errno));
      CONSOLE.Warning(1,
        "Error merging data; more available disk space may be needed--"
        "will retry in %d seconds.", WRITE_RETRY_INTERVAL);
      m_write_failed = true;
      m_write_tried = now;
      goto done;
    }
    m_write_failed = false;
    remain -= nio;
    pos += nio;
    if( (dt_datalen_t)pos > dst->bf_size )
      dst->bf_size = pos;
  }

  if( dst->bf_size == dst->bf_length ) _btf_close(dst);  // will reopen RO
 delsrc:
  _btf_close(src);
  sprintf(buf, "%s%c%s", m_staging_path, PATH_SP, src->bf_filename);
  if(*cfg_verbose) CONSOLE.Debug("Delete file \"%s\"", buf);
  diskaccess = true;
  if( remove(buf) < 0 ){
    CONSOLE.Warning(2, "error deleting file \"%s\":  %s", buf, strerror(errno));
  }
  dst->bf_next = src->bf_next;

  if( m_stagecount > 0 &&
      0==strncmp(m_stagedir, src->bf_filename, strlen(m_stagedir)) ){
    m_stagecount--;
    if( 0==m_stagecount ){
      f_remove = 1;
      m_stagedir[0] = '\0';
    }
  }else f_remove = 1;
  if( f_remove ){
    struct stat sb;
    DIR *dp;
    struct dirent *dirp;
    sprintf(buf, "%s%c", m_staging_path, PATH_SP);
    strncat(buf, src->bf_filename, m_fsizelen);
    if( 0==stat(buf, &sb) && S_ISDIR(sb.st_mode) && (dp = opendir(buf)) ){
      while( (dirp = readdir(dp)) ){
        if( 0!=strcmp(dirp->d_name, ".") && 0!=strcmp(dirp->d_name, "..") ){
          f_remove = 0;
          break;
        }
      }
      closedir(dp);
      if( f_remove ){
        if(*cfg_verbose) CONSOLE.Debug("Remove dir \"%s\"", buf);
        if( remove(buf) < 0 ){
          CONSOLE.Warning(2, "warn, remove directory \"%s\" failed:  %s", buf,
            strerror(errno));
        }
      }
    }
  }

  delete src;
  result = 0;
 done:
  if( diskaccess ) DiskAccess();
  return result;
}

// Identify a file that can be merged, and do it
int btFiles::FindAndMerge(int findall, int dostaging)
{
  BTFILE *pbf = m_btfhead;
  int merged = 0;

  for( ; pbf; pbf = dostaging ? pbf->bf_next : pbf->bf_nextreal ){
    while( !pbf->bf_flag_staging && pbf->bf_next &&
        pbf->bf_next->bf_flag_staging && pbf->bf_size < pbf->bf_length &&
        pbf->bf_offset + pbf->bf_size >= pbf->bf_next->bf_offset ){
      if( findall ) CONSOLE.Interact_n(".");
      if( MergeStaging(pbf) < 0 ) goto done;
      merged = 1;
      if( !findall ) goto done;
    }
  }
  m_need_merge = 0;

 done:
  return merged;
}

/* Of the choices presented, select a piece that will help toward merging
   staged data.
   choices:     The preferred set from which to choose.
   available:   The perhaps wider set of all pieces which could be chosen.
   preference:  Prefer this piece in a final random selection.
*/
bt_index_t btFiles::ChoosePiece(const Bitfield &choices,
  const Bitfield &available, bt_index_t preference) const
{
  Bitfield needs(BTCONTENT.GetNPieces()), needsnext(BTCONTENT.GetNPieces());
  BTFILE *pbf = m_btfhead, *pbt;
  bt_index_t idx;
  int found;

  for( ; pbf; pbf = pbf->bf_nextreal ){
    if( pbf->bf_next && pbf->bf_next->bf_flag_staging ){
      // next piece of this file helps fill a merge gap
      idx = (pbf->bf_offset + pbf->bf_size) / BTCONTENT.GetPieceLength();
      if( available.IsSet(idx) &&
          pbf->bf_next->bf_offset <=
            pbf->bf_offset + pbf->bf_size + BTCONTENT.GetPieceLength() ){
        // piece will fill a merge gap
        return idx;
      }
      if( choices.IsSet(preference) &&
          (dt_datalen_t)preference * BTCONTENT.GetPieceLength() >=
            pbf->bf_offset &&
          (dt_datalen_t)preference * BTCONTENT.GetPieceLength() <
            pbf->bf_next->bf_offset ){
        // preference helps fill a merge gap
        return preference;
      }
      // mark pieces from this merge gap for selection
      for( ;
          (dt_datalen_t)idx * BTCONTENT.GetPieceLength() <
            pbf->bf_next->bf_offset;
          idx++ ){
        if( choices.IsSet(idx) ) needs.Set(idx);
      }
      if( needs.IsEmpty() ){
        // work on the next staging gap of this file as a secondary priority
        found = 0;
        for( pbt = pbf->bf_next;
             !found && pbt->bf_next && pbt->bf_next->bf_flag_staging;
             pbt = pbt->bf_next ){
          idx = (pbt->bf_offset + pbt->bf_size) / BTCONTENT.GetPieceLength();
          for( ;
              (dt_datalen_t)idx * BTCONTENT.GetPieceLength() <
                pbt->bf_next->bf_offset;
              idx++ ){
            if( choices.IsSet(idx) ){
              needsnext.Set(idx);
              found = 1;
            }
          }
        }
      }  // if needs.IsEmpty()
    }
  }
  return needs.IsEmpty() ?
            ( (needsnext.IsEmpty() || needsnext.IsSet(preference)) ?
                preference : needsnext.Random() ) :
            needs.Random();
}

int btFiles::_btf_destroy()
{
  BTFILE *pbf, *pbf_next;
  for( pbf = m_btfhead; pbf; pbf = pbf_next ){
    pbf_next = pbf->bf_next;
    delete pbf;
  }
  m_btfhead = (BTFILE *)0;
  m_total_files_length = 0;
  m_total_opened = 0;
  return 0;
}

int btFiles::ExtendFile(BTFILE *pbf)
{
  dt_datalen_t newsize, length;
  int retval;

  if( pbf->bf_next )
    newsize = pbf->bf_next->bf_offset - pbf->bf_offset;
  else newsize = pbf->bf_length;

  if( (!pbf->bf_flag_opened || pbf->bf_flag_readonly) &&
      _btf_open(pbf, 1) < 0 ){
    CONSOLE.Warning(1, "error, failed to open file \"%s\" for writing:  %s",
      pbf->bf_filename, strerror(errno));
    return pbf->bf_length ? -1 : 0;
  }
  if( pbf->bf_length == 0 ){
    _btf_close(pbf);
    return 0;
  }

  if( *cfg_allocate == DT_ALLOC_FULL ){
    off_t pos = pbf->bf_size;
    if( fseeko(pbf->bf_fp, pos, SEEK_SET) < 0 ){
      CONSOLE.Warning(1, "error, failed to seek to %llu on file \"%s\":  %s",
        (unsigned long long)pos, pbf->bf_filename, strerror(errno));
      return -1;
    }
    length = newsize - pbf->bf_size;
  }else length = newsize;

  retval = length ? _btf_ftruncate(fileno(pbf->bf_fp), length) : 0;
  if( retval < 0 ){
    CONSOLE.Warning(1, "error, allocate file \"%s\" failed:  %s",
      pbf->bf_filename, strerror(errno));
  }else pbf->bf_size = newsize;

  _btf_close(pbf);
  return retval;
}

int btFiles::_btf_ftruncate(int fd, dt_datalen_t length)
{
  off_t offset = length;

  if( length == 0 ) return 0;

  if( *cfg_allocate == DT_ALLOC_FULL ){  // preallocate to disk (-a)
    char *c = new char[OPT_IO_SIZE];
    if( !c ){
      errno = ENOMEM;
      return -1;
    }
    memset(c, 0, OPT_IO_SIZE);
    int r = 0, wlen;
    dt_datalen_t len = 0;
    for( int i=0; len < length; i++ ){
      if( len + OPT_IO_SIZE > length ) wlen = (int)(length - len);
      else wlen = OPT_IO_SIZE;
      if( 0 == i % 100 ) CONSOLE.Interact_n(".");
      if( (r = write(fd, c, wlen)) < 0 ) return r;
      len += wlen;
    }
    delete []c;
    return r;
  }

  // create sparse file
#ifdef WINDOWS
  char c = (char)0;
  if( lseek(fd, offset - 1, SEEK_SET) < 0 ) return -1;
  return write(fd, &c, 1);
#else
  // ftruncate() not allowed on [v]fat under linux
  int retval = ftruncate(fd, offset);
  if( retval < 0 ){
    char c = (char)0;
    if( lseek(fd, offset - 1, SEEK_SET) < 0 ) return -1;
    return write(fd, &c, 1);
  }
  else return retval;
#endif
}

int btFiles::_btf_recurses_directory(const char *cur_path, BTFILE **plastnode)
{
  char full_cur[MAXPATHLEN];
  char fn[MAXPATHLEN];
  struct stat sb;
  struct dirent *dirp;
  DIR *dp;
  BTFILE *pbf;

  if( !getcwd(full_cur, MAXPATHLEN) ) return -1;

  if( cur_path ){
    strcpy(fn, full_cur);
    if( MAXPATHLEN <= snprintf(full_cur, MAXPATHLEN, "%s%c%s", fn, PATH_SP,
                               cur_path) ){
      errno = ENAMETOOLONG;
      return -1;
    }
  }

  if( !(dp = opendir(full_cur)) ){
    CONSOLE.Warning(1, "error, open directory \"%s\" failed:  %s",
      cur_path, strerror(errno));
    return -1;
  }

  while( (dirp = readdir(dp)) ){
    if( 0 == strcmp(dirp->d_name, ".") ||
        0 == strcmp(dirp->d_name, "..") ){
      continue;
    }
    if( cur_path ){
      if( MAXPATHLEN < snprintf(fn, MAXPATHLEN, "%s%c%s", cur_path, PATH_SP,
                               dirp->d_name) ){
        CONSOLE.Warning(1, "error, pathname too long");
        errno = ENAMETOOLONG;
        return -1;
      }
    }else{
      strcpy(fn, dirp->d_name);
    }

    if( stat(fn, &sb) < 0 ){
      CONSOLE.Warning(1, "error, stat \"%s\" failed:  %s", fn, strerror(errno));
      return -1;
    }

    if( S_IFREG & sb.st_mode ){
      pbf = new BTFILE;
#ifndef WINDOWS
      if( !pbf ){
        closedir(dp);
        errno = ENOMEM;
        return -1;
      }
#endif
      pbf->bf_filename = new char[strlen(fn) + 1];
#ifndef WINDOWS
      if( !pbf->bf_filename ){
        closedir(dp);
        errno = ENOMEM;
        return -1;
      }
#endif
      strcpy(pbf->bf_filename, fn);

      pbf->bf_offset = m_total_files_length;
      pbf->bf_length = pbf->bf_size = sb.st_size;
      m_total_files_length += sb.st_size;

      if( *plastnode ){
        (*plastnode)->bf_next = (*plastnode)->bf_nextreal = pbf;
      }else m_btfhead = pbf;

      *plastnode = pbf;

    }else if( S_IFDIR & sb.st_mode ){
      if( _btf_recurses_directory(fn, plastnode) < 0 ){
        closedir(dp);
        return -1;
      }
    }else{
      CONSOLE.Warning(1, "error, \"%s\" is not a directory or regular file.",
        fn);
      closedir(dp);
      errno = EINVAL;
      return -1;
    }
  }  // end while
  closedir(dp);
  return 0;
}

// Only creates the path, not the final dir or file.
int btFiles::MkPath(const char *pathname)
{
  struct stat sb;
  char sp[strlen(pathname)+1];
  char *p;

  strcpy(sp, pathname);
  p = sp;
  if( PATH_SP == *p ) p++;

  for( ; *p; p++ ){
    if( PATH_SP == *p ){
      *p = '\0';
      if( stat(sp, &sb) < 0 ){
        if( ENOENT == errno ){
          if( mkdir(sp, 0755) < 0 ) return -1;
        }else return -1;
      }
      *p = PATH_SP;
    }
  }
  return 0;
}

int btFiles::BuildFromFS(const char *pathname)
{
  int result = -1;
  struct stat sb;
  BTFILE *pbf = (BTFILE *)0;
  BTFILE *lastnode = (BTFILE *)0;

  if( stat(pathname, &sb) < 0 ){
    CONSOLE.Warning(1, "error, stat file \"%s\" failed:  %s",
      pathname, strerror(errno));
    goto done;
  }

  if( S_IFREG & sb.st_mode ){
    pbf = new BTFILE;
#ifndef WINDOWS
    if( !pbf ){
      errno = ENOMEM;
      goto done;
    }
#endif
    pbf->bf_offset = 0;
    pbf->bf_length = pbf->bf_size = m_total_files_length = sb.st_size;
    pbf->bf_filename = new char[strlen(pathname) + 1];
#ifndef WINDOWS
    if( !pbf->bf_filename ){
      errno = ENOMEM;
      goto done;
    }
#endif
    strcpy(pbf->bf_filename, pathname);
    m_btfhead = pbf;
  }else if( S_IFDIR & sb.st_mode ){
    char wd[MAXPATHLEN];
    if( !getcwd(wd, MAXPATHLEN) ) goto done;
    m_directory = new char[strlen(pathname) + 1];
#ifndef WINDOWS
    if( !m_directory ){
      errno = ENOMEM;
      goto done;
    }
#endif
    strcpy(m_directory, pathname);

    if( chdir(m_directory) < 0 ){
      CONSOLE.Warning(1, "error, change work directory to \"%s\" failed:  %s",
        m_directory, strerror(errno));
      goto done;
    }

    if( _btf_recurses_directory((const char *)0, &lastnode) < 0 ) goto done;
    if( chdir(wd) < 0 ) goto done;
  }else{
    CONSOLE.Warning(1, "error, \"%s\" is not a directory or regular file.",
      pathname);
    errno = EINVAL;
    goto done;
  }
  result = 0;
 done:
  DiskAccess();
  return result;
}

int btFiles::BuildFromMI(const char *metabuf, const size_t metabuf_len,
  const char *saveas, bool exam_only)
{
  char path[MAXPATHLEN];
  const char *s, *p;
  size_t r, q, n;
  int64_t t;
  int f_warned = 0, i = 0;
  BTFILE *pbt;

  if( !decode_query(metabuf, metabuf_len, "info|name", &s, &q, (int64_t *)0,
        DT_QUERY_STR) || MAXPATHLEN <= q ){
    errno = EINVAL;
    return -1;
  }

  cfg_convert_filenames.Lock();

  memcpy(path, s, q);
  path[q] = '\0';
  if( !exam_only &&
      (PATH_SP == path[0] || '/' == path[0] || 0==strncmp("..", path, 2)) ){
    CONSOLE.Warning(1, "error, unsafe path \"%s\" in torrent data", path);
    errno = EINVAL;
    return -1;
  }

  r = decode_query(metabuf, metabuf_len, "info|files", (const char **)0, &q,
                   (int64_t *)0, DT_QUERY_POS);

  if( r ){  // torrent contains multiple files
    BTFILE *pbf_last = (BTFILE *)0;
    BTFILE *pbf = (BTFILE *)0;
    size_t dl;
    if( decode_query(metabuf, metabuf_len, "info|length", (const char **)0,
                     (size_t *)0, (int64_t *)0, DT_QUERY_INT) ){
      errno = EINVAL;
      return -1;
    }

    if( saveas ){
      m_directory = new char[strlen(saveas) + 1];
#ifndef WINDOWS
      if( !m_directory ){
        errno = ENOMEM;
        return -1;
      }
#endif
      strcpy(m_directory, saveas);
    }else{
      int f_conv;
      char *tmpfn = new char[strlen(path)*2+5];
#ifndef WINDOWS
      if( !tmpfn ){
        errno = ENOMEM;
        return -1;
      }
#endif
      if( (f_conv = ConvertFilename(tmpfn, path, strlen(path)*2+5)) ){
        if( *cfg_convert_filenames ){
          m_directory = new char[strlen(tmpfn) + 1];
#ifndef WINDOWS
          if( !m_directory ){
            delete []tmpfn;
            errno = ENOMEM;
            return -1;
          }
#endif
          strcpy(m_directory, tmpfn);
        }else{
          CONSOLE.Warning(3,
            "Dir name contains non-printable characters; use -T to convert.");
          f_warned = 1;
        }
      }
      delete []tmpfn;
      if( !f_conv || !*cfg_convert_filenames ){
        m_directory = new char[strlen(path) + 1];
#ifndef WINDOWS
        if( !m_directory ){
          errno = ENOMEM;
          return -1;
        }
#endif
        strcpy(m_directory, path);
      }
    }

    /* now r saved the pos of files list. q saved list length */
    p = metabuf + r + 1;
    q--;
    for( ; q && 'e' != *p; p += dl, q -= dl ){
      if( !(dl = decode_dict(p, q, (const char *)0)) ||
          !decode_query(p, dl, "length", (const char **)0, (size_t *)0, &t,
                        DT_QUERY_INT) ){
        errno = EINVAL;
        return -1;
      }
      pbf = new BTFILE;
#ifndef WINDOWS
      if( !pbf ){
        errno = ENOMEM;
        return -1;
      }
#endif
      m_nfiles++;
      pbf->bf_offset = m_total_files_length;
      pbf->bf_length = t;
      m_total_files_length += t;
      r = decode_query(p, dl, "path", (const char **)0, &n, (int64_t *)0,
                       DT_QUERY_POS);
      if( !r || !decode_list2path(p + r, n, path, sizeof(path)) ){
        CONSOLE.Warning(1,
          "error, invalid path in torrent data for file %lu at offset %llu",
          (unsigned long)m_nfiles, (unsigned long long)pbf->bf_offset);
        delete pbf;
        errno = EINVAL;
        return -1;
      }
      if( !exam_only &&
          (PATH_SP == path[0] || '/' == path[0] || 0==strncmp("..", path, 2)) ){
        CONSOLE.Warning(1,
          "error, unsafe path \"%s\" in torrent data for file %lu",
          path, (unsigned long)m_nfiles);
        delete pbf;
        errno = EINVAL;
        return -1;
      }

      int f_conv;
      char *tmpfn = new char[strlen(path)*2+5];
#ifndef WINDOWS
      if( !tmpfn ){
        errno = ENOMEM;
        return -1;
      }
#endif
      if( (f_conv = ConvertFilename(tmpfn, path, strlen(path)*2+5)) ){
        if( *cfg_convert_filenames ){
          pbf->bf_filename = new char[strlen(tmpfn) + 1];
#ifndef WINDOWS
          if( !pbf->bf_filename ){
            delete []tmpfn;
            errno = ENOMEM;
            return -1;
          }
#endif
          strcpy(pbf->bf_filename, tmpfn);
        }else if( !f_warned ){
          CONSOLE.Warning(3,
            "Filename contains non-printable characters; use -T to convert.");
          f_warned = 1;
        }
      }
      delete []tmpfn;
      if( !f_conv || !*cfg_convert_filenames ){
        pbf->bf_filename = new char[strlen(path) + 1];
#ifndef WINDOWS
        if( !pbf->bf_filename ){
          errno = ENOMEM;
          return -1;
        }
#endif
        strcpy(pbf->bf_filename, path);
      }
      if( pbf_last ){
        pbf_last->bf_next = pbf_last->bf_nextreal = pbf;
      }else m_btfhead = pbf;
      pbf_last = pbf;
    }
  }else{  // torrent contains a single file
    if( !decode_query(metabuf, metabuf_len, "info|length",
                      (const char **)0, (size_t *)0, &t, DT_QUERY_INT) ){
      errno = EINVAL;
      return -1;
    }
    m_btfhead = new BTFILE;
#ifndef WINDOWS
    if( !m_btfhead){
      errno = ENOMEM;
      return -1;
    }
#endif
    m_nfiles++;
    m_btfhead->bf_offset = 0;
    m_btfhead->bf_length = m_total_files_length = t;
    if( saveas ){
      m_btfhead->bf_filename = new char[strlen(saveas) + 1];
#ifndef WINDOWS
      if( !m_btfhead->bf_filename ){
        errno = ENOMEM;
        return -1;
      }
#endif
      strcpy(m_btfhead->bf_filename, saveas);
    }else if( *cfg_convert_filenames ){
      char *tmpfn = new char[strlen(path)*2+5];
#ifndef WINDOWS
      if( !tmpfn ){
        errno = ENOMEM;
        return -1;
      }
#endif
      ConvertFilename(tmpfn, path, strlen(path)*2+5);
      m_btfhead->bf_filename = new char[strlen(tmpfn) + 1];
#ifndef WINDOWS
      if( !m_btfhead->bf_filename ){
        delete []tmpfn;
        errno = ENOMEM;
        return -1;
      }
#endif
      strcpy(m_btfhead->bf_filename, tmpfn);
      delete []tmpfn;
    }else{
      m_btfhead->bf_filename = new char[strlen(path) + 1];
#ifndef WINDOWS
      if( !m_btfhead->bf_filename ){
        errno = ENOMEM;
        return -1;
      }
#endif
      strcpy(m_btfhead->bf_filename, path);
    }
  }

  m_file = new BTFILE *[m_nfiles];
  if( !m_file ){
    CONSOLE.Warning(1, "error, failed to allocate memory for files list");
    errno = ENOMEM;
    return -1;
  }
  for( i=0, pbt = m_btfhead; pbt; pbt = pbt->bf_nextreal ){
    m_file[i++] = pbt;
  }
  return 0;
}

int btFiles::SetupFiles(const char *torrentid, bool check_only)
{
  int result = -1;
  DIR *dp, *subdp;
  struct dirent *dirp;
  struct stat sb;
  BTFILE *pbf, *pbt;
  dt_datalen_t offset;
  char fn[MAXPATHLEN], *tmp;
  bool files_exist = false;

  m_fsizelen = sprintf(fn, "%llu", (unsigned long long)m_total_files_length);
  if( !(m_torrent_id = new char[strlen(torrentid) + 1]) ||
      !(pBFPieces = new Bitfield(BTCONTENT.GetNPieces())) ||
      !(m_staging_path = new char[strlen(torrentid) + 10]) ||
      !(m_stagedir = new char[m_fsizelen + 1]) ){
    CONSOLE.Warning(1, "error, failed to allocate memory");
    errno = ENOMEM;
    return -1;
  }
  strcpy(m_torrent_id, torrentid);
  sprintf(m_staging_path, "%s%c%s", *cfg_staging_dir, PATH_SP, m_torrent_id);
  cfg_staging_dir.Lock();
  m_stagedir[0] = '\0';

  // Identify existing staging files.
  if( !(dp = opendir(m_staging_path)) && !check_only ){
    int err = errno;
    if( stat(m_staging_path, &sb) == 0 ){
      CONSOLE.Warning(1, "error, cannot access staging directory \"%s\":  %s",
        m_staging_path, strerror(err));
      goto done;
    }
  }else while( dp && (dirp = readdir(dp)) ){
    if( strlen(dirp->d_name) == m_fsizelen ){
      if( MAXPATHLEN <= snprintf(fn, MAXPATHLEN, "%s%c%s",
                                 m_staging_path, PATH_SP, dirp->d_name) ||
          stat(fn, &sb) < 0 || !S_ISDIR(sb.st_mode) ||
          !(subdp = opendir(fn)) ){
        continue;
      }
      m_stagecount = 0;
      strcpy(m_stagedir, dirp->d_name);
      while( (dirp = readdir(subdp)) ){
        if( 0==strncmp(m_torrent_id, dirp->d_name, strlen(m_torrent_id)) &&
            dirp->d_name[strlen(m_torrent_id)] == '-' &&
            MAXPATHLEN > snprintf(fn, MAXPATHLEN, "%s%c%s%c%s",
                 m_staging_path, PATH_SP, m_stagedir, PATH_SP, dirp->d_name) &&
            0==stat(fn, &sb) && S_ISREG(sb.st_mode) ){
          offset = strtoull(dirp->d_name + strlen(m_torrent_id) + 1, &tmp, 10);
          if( tmp != dirp->d_name + strlen(m_torrent_id) + m_fsizelen + 1 )
            continue;

          if( !(pbf = new BTFILE) ||
              !(pbf->bf_filename = new char[strlen(m_stagedir) +
                strlen(dirp->d_name) + 2]) ){
            CONSOLE.Warning(1,
              "error, failed to allocate memory for staging file");
            if( pbf ) delete pbf;
            errno = ENOMEM;
            goto done;
          }
          sprintf(pbf->bf_filename, "%s%c%s", m_stagedir, PATH_SP,
            dirp->d_name);
          if( stat(fn, &sb) < 0 ){  // fn already contains path+filename
            CONSOLE.Warning(1, "error, check staging file \"%s\":  %s", fn,
              strerror(errno));
            delete pbf;
            continue;
          }
          pbf->bf_flag_staging = 1;
          pbf->bf_offset = offset;
          pbf->bf_size = sb.st_size;
          if(*cfg_verbose) CONSOLE.Debug("Found staging file %s size %llu",
            pbf->bf_filename, (unsigned long long)pbf->bf_size);
          m_stagecount++;
          if( pbf->bf_size > 0 ) files_exist = true;

          for( pbt = m_btfhead; pbt->bf_next; pbt = pbt->bf_next ){
            if( pbf->bf_offset < pbt->bf_next->bf_offset ) break;
          }
          pbf->bf_next = pbt->bf_next;
          pbt->bf_next = pbf;
          pbf->bf_nextreal = pbt->bf_nextreal;
        }
      }
      closedir(subdp);
    }
  }
  if( dp ) closedir(dp);

  // Check for main torrent content files.
  for( pbt = m_btfhead ; pbt; pbt = pbt->bf_nextreal ){
    if( m_directory ){
      if( MAXPATHLEN <= snprintf(fn, MAXPATHLEN, "%s%c%s",
          m_directory, PATH_SP, pbt->bf_filename) ){
        errno = ENAMETOOLONG;
        goto done;
      }
    }else strcpy(fn, pbt->bf_filename);

    if( stat(fn, &sb) < 0 ){
      if( ENOENT != errno ){
        CONSOLE.Warning(1, "error, stat file \"%s\" failed:  %s", fn,
          strerror(errno));
        goto done;
      }
    }else{
      if( !(S_IFREG & sb.st_mode) ){
        CONSOLE.Warning(1, "error, file \"%s\" is not a regular file.", fn);
        errno = EINVAL;
        goto done;
      }
      if( (dt_datalen_t)sb.st_size > pbt->bf_length ){
        CONSOLE.Warning(1, "error, file \"%s\" size is too big; should be %llu",
          fn, (unsigned long long)pbt->bf_length);
        errno = EINVAL;
        goto done;
      }
      pbt->bf_size = sb.st_size;
      if( pbt->bf_size > 0 ) files_exist = true;
    }
  }

  result = files_exist ? 1 : 0;

 done:
  DiskAccess();
  return result;
}

int btFiles::CreateFiles()
{
  int result = -1;
  struct stat sb;
  BTFILE *pbf;
  dt_datalen_t idxoff, fend, idxend;

  cfg_allocate.Lock();
  CfgAllocate(&cfg_allocate);  // set default info

  if( (*cfg_allocate == DT_ALLOC_NONE ||
        !BTCONTENT.pBMasterFilter->IsEmpty()) &&
      stat(m_staging_path, &sb) < 0 ){
    if( ENOENT == errno ){
      if( MkPath(m_staging_path) < 0 || mkdir(m_staging_path, 0755) < 0 ){
        CONSOLE.Warning(1, "error, create staging directory \"%s\" failed:  %s",
          m_staging_path, strerror(errno));
        goto done;
      }
    }else{
      CONSOLE.Warning(1, "error, cannot access staging directory \"%s\":  %s",
        m_staging_path, strerror(errno));
      goto done;
    }
  }

  // Create/allocate files.
  if( *cfg_allocate == DT_ALLOC_FULL || *cfg_allocate == DT_ALLOC_SPARSE ){
    CONSOLE.Interact_n();
    CONSOLE.Interact_n("Allocating files");
    MergeAll();
    while( ExtendAll() >= 0 && MergeAll() );
  }else m_need_merge = 1;

  // Set up map of pieces that are available in the files.
  pbf = m_btfhead;
  for( bt_index_t idx = 0; idx < BTCONTENT.GetNPieces() && pbf; idx++ ){
    idxoff = idx * (dt_datalen_t)BTCONTENT.GetPieceLength();
    if( idxoff < pbf->bf_offset ) continue;
    fend = pbf->bf_offset + pbf->bf_size - (pbf->bf_size ? 1 : 0);
    while( pbf && (pbf->bf_size == 0 || (idxoff > fend && pbf->bf_next)) ){
      pbf = pbf->bf_next;
      if( pbf ) fend = pbf->bf_offset + pbf->bf_size - 1;
    }
    if( !pbf || idxoff > fend ) break;  // no more files

    if( idxoff >= pbf->bf_offset ){
      idxend = idxoff + BTCONTENT.GetPieceLength(idx) - 1;
      while( pbf && idxend > fend && pbf->bf_next &&
             pbf->bf_next->bf_offset <= fend + 1 ){
        pbf = pbf->bf_next;
        if( pbf ) fend = pbf->bf_offset + pbf->bf_size - (pbf->bf_size ? 1 : 0);
      }
      if( !pbf ) break;  // no more files
      if( idxend <= fend ) pBFPieces->Set(idx);
    }
  }
  if(*cfg_verbose)
    CONSOLE.Debug("Files contain %d pieces", (int)pBFPieces->Count());

  result = pBFPieces->IsEmpty() ? 0 : 1;

 done:
  DiskAccess();
  return result;
}

int btFiles::ExtendAll()
{
  BTFILE *pbf = m_btfhead;
  int i;
  Bitfield tmpFilter;

  for( i = 1; pbf; pbf = pbf->bf_nextreal, i++ ){
    if( pbf->bf_size > 0 && pbf->bf_size >= pbf->bf_length ) continue;
    if( *cfg_file_to_download ){
      SetFilter(i, &tmpFilter, BTCONTENT.GetPieceLength());
      tmpFilter.Invert();
      tmpFilter.And(BTCONTENT.pBMasterFilter);
      if( !tmpFilter.IsEmpty() )
        continue;
    }
    if( *cfg_allocate != DT_ALLOC_FULL ) CONSOLE.Interact_n(".");
    if( ExtendFile(pbf) < 0 ) return -1;
  }
  return 0;
}

void btFiles::PrintOut(bool show_completion) const
{
  BTFILE *p = m_btfhead;
  dt_count_t id = 0;
  CONSOLE.Print("");
  CONSOLE.Print("FILES INFO");
  Bitfield tmpBitfield, tmpFilter;
  if( m_directory ) CONSOLE.Print("Directory: %s", m_directory);
  for( ; p; p = p->bf_nextreal ){
    ++id;
    CONSOLE.Print_n();
    CONSOLE.Print_n("<%d> %s%s [%llu]", (int)id, m_directory ? " " : "",
      p->bf_filename, (unsigned long long)p->bf_length);
    if( show_completion ){
      BTCONTENT.SetTmpFilter(id, &tmpFilter);
      tmpBitfield = *BTCONTENT.pBF;
      tmpBitfield.Except(tmpFilter);
      CONSOLE.Print_n(" %d/%d (%d%%)",
        (int)tmpBitfield.Count(), (int)GetFilePieces(id),
        GetFilePieces(id) ?
          100 * tmpBitfield.Count() / GetFilePieces(id) : 100);
    }
  }
  CONSOLE.Print("Total: %lu MB",
    (unsigned long)(m_total_files_length/1024/1024));
}

int btFiles::FillMetaInfo(FILE *fp)
{
  BTFILE *p;
  const char *refname, *s;
  char path[MAXPATHLEN];

  refname = m_directory ? m_directory : m_btfhead->bf_filename;
  while( (s = strchr(refname, PATH_SP)) && *(s + 1) ){
    refname = s + 1;
  }
  if( m_directory && '.' == *refname ){
    char dir[MAXPATHLEN];
    if( getcwd(dir, sizeof(dir)) && 0==chdir(m_directory) ){
      if( getcwd(path, sizeof(path)) ){
        refname = path;
        while( (s = strchr(refname, PATH_SP)) && *(s + 1) ){
          refname = s + 1;
        }
      }
      chdir(dir);
    }
  }
  if( '/' == *refname || '\0' == *refname || '.' == *refname ){
    CONSOLE.Warning(1, "error, inappropriate file or directory name \"%s\"",
      m_directory ? m_directory : m_btfhead->bf_filename);
    errno = EINVAL;
    return 0;
  }

  if( m_directory ){
    // multiple files
    if( bencode_str("files", fp) != 1 ) return 0;

    if( bencode_begin_list(fp) != 1 ) return 0;

    for( p = m_btfhead; p; p = p->bf_next ){
      if( bencode_begin_dict(fp) != 1 ) return 0;

      if( bencode_str("length", fp) != 1 ) return 0;
      if( bencode_int(p->bf_length, fp) != 1 ) return 0;

      if( bencode_str("path", fp) != 1 ) return 0;
      if( bencode_path2list(p->bf_filename, fp) != 1 ) return 0;

      if( bencode_end_dict_list(fp) != 1 ) return 0;
    }

    if( bencode_end_dict_list(fp) != 1 ) return 0;

    if( bencode_str("name", fp) != 1 ) return 0;
    return bencode_str(refname, fp);
  }else{
    if( bencode_str("length", fp) != 1 ) return 0;
    if( bencode_int(m_btfhead->bf_length, fp) != 1 ) return 0;

    if( bencode_str("name", fp) != 1 ) return 0;
    return bencode_str(refname, fp);
  }
  return 0;
}


void btFiles::SetFilter(dt_count_t nfile, Bitfield *pFilter,
  bt_length_t pieceLength)
{
  BTFILE *p;
  dt_count_t id = 0;
  bt_index_t index;

  if( nfile == 0 || nfile > m_nfiles ){
    pFilter->Clear();
    return;
  }

  for( p = m_btfhead; p; p = p->bf_nextreal ){
    if( ++id == nfile ){
      if( 0 == p->bf_length ){
        p->bf_npieces = 0;
        pFilter->SetAll();
        return;
      }
      bt_index_t start, stop;
      start = p->bf_offset / pieceLength;
      stop  = (p->bf_offset + p->bf_length) / pieceLength;
      // calculation is off if file ends on a piece boundary
      if( stop > start && 0 == (p->bf_offset + p->bf_length) % pieceLength )
        --stop;
      p->bf_npieces = stop - start + 1;

      if( p->bf_npieces <= pFilter->NBits() / 2 ){
        pFilter->SetAll();
        for( index = start; index <= stop; index++ ){
          pFilter->UnSet(index);
        }
      }else{
        pFilter->Clear();
        for( index = 0; index < start; index++ ){
          pFilter->Set(index);
        }
        for( index = stop + 1; index < pFilter->NBits(); index++ ){
          pFilter->Set(index);
        }
      }
      break;
    }
  }
}

const char *btFiles::GetFileName(dt_count_t nfile) const
{
  if( nfile && nfile <= m_nfiles )
    return m_file[nfile-1]->bf_filename;
  return (char *)0;
}

dt_datalen_t btFiles::GetFileSize(dt_count_t nfile) const
{
  if( nfile && nfile <= m_nfiles )
    return m_file[nfile-1]->bf_length;
  return 0;
}

// Returns the number of pieces in the file
bt_index_t btFiles::GetFilePieces(dt_count_t nfile) const
{
  if( nfile && nfile <= m_nfiles )
    return m_file[nfile-1]->bf_npieces;
  return 0;
}

int btFiles::ConvertFilename(char *dst, const char *src, int size)
{
  int retval=0, i, j, f_print=0, f_punct=0;

  for( i = j = 0; src[i] != '\0' && j < size-2; i++ ){
    if( isprint(src[i]) ){
      if( ispunct(src[i]) ) f_punct = 1;
      else f_punct = 0;
      if( j && !f_print && !f_punct ){
        sprintf(dst + j, "_");
        j++;
      }
      dst[j++] = src[i];
      f_print = 1;
    }else{
      if( f_print && !f_punct ){
        sprintf(dst + j, "_");
        j++;
      }
      snprintf(dst + j, 3, "%.2X", (unsigned char)src[i]);
      j += 2;
      f_print = f_punct = 0;
      if( !retval ) retval = 1;
    }
  }
  dst[j] = '\0';
  return retval;
}

const char *btFiles::GetDataName() const
{
  return m_directory ? m_directory : m_btfhead->bf_filename;
}

