/* 
   Copyright (C) Andrew Tridgell 1996
   Copyright (C) Paul Mackerras 1996
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "rsync.h"

extern int csum_length;

extern int verbose;
extern int am_server;
extern int always_checksum;
extern time_t starttime;

extern int remote_version;

extern char *backup_suffix;
extern char *tmpdir;

extern int whole_file;
extern int block_size;
extern int update_only;
extern int make_backups;
extern int preserve_links;
extern int preserve_hard_links;
extern int preserve_perms;
extern int preserve_devices;
extern int preserve_uid;
extern int preserve_gid;
extern int preserve_times;
extern int dry_run;
extern int ignore_times;
extern int recurse;
extern int delete_mode;
extern int cvs_exclude;
extern int am_root;
extern int relative_paths;

/*
  free a sums struct
  */
static void free_sums(struct sum_struct *s)
{
  if (s->sums) free(s->sums);
  free(s);
}


/*
 * delete a file or directory. If force_delet is set then delete 
 * recursively 
 */
static int delete_file(char *fname)
{
	DIR *d;
	struct dirent *di;
	char buf[MAXPATHLEN];
	extern int force_delete;
	struct stat st;
	int ret;

	if (do_unlink(fname) == 0 || errno == ENOENT) return 0;

#if SUPPORT_LINKS
	ret = lstat(fname, &st);
#else
	ret = stat(fname, &st);
#endif
	if (ret) {
		fprintf(FERROR,"stat(%s) : %s\n", fname, strerror(errno));
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		fprintf(FERROR,"unlink(%s) : %s\n", fname, strerror(errno));
		return -1;
	}

	if (do_rmdir(fname) == 0 || errno == ENOENT) return 0;
	if (!force_delete || errno != ENOTEMPTY) {
		fprintf(FERROR,"rmdir(%s) : %s\n", fname, strerror(errno));
		return -1;
	}

	/* now we do a recsursive delete on the directory ... */
	d = opendir(fname);
	if (!d) {
		fprintf(FERROR,"opendir(%s): %s\n",
			fname,strerror(errno));
		return -1;
	}

	for (di=readdir(d); di; di=readdir(d)) {
		if (strcmp(di->d_name,".")==0 ||
		    strcmp(di->d_name,"..")==0)
			continue;
		strncpy(buf, fname, (MAXPATHLEN-strlen(di->d_name))-2);
		strcat(buf, "/");
		strcat(buf, di->d_name);
		buf[MAXPATHLEN-1] = 0;
		if (verbose > 0)
			fprintf(FINFO,"deleting %s\n", buf);
		if (delete_file(buf) != 0) {
			closedir(d);
			return -1;
		}
	}	

	closedir(d);
	
	if (do_rmdir(fname) != 0) {
		fprintf(FERROR,"rmdir(%s) : %s\n", fname, strerror(errno));
		return -1;
	}

	return 0;
}

/*
  send a sums struct down a fd
  */
static void send_sums(struct sum_struct *s,int f_out)
{
  int i;

  /* tell the other guy how many we are going to be doing and how many
     bytes there are in the last chunk */
  write_int(f_out,s?s->count:0);
  write_int(f_out,s?s->n:block_size);
  write_int(f_out,s?s->remainder:0);
  if (s)
    for (i=0;i<s->count;i++) {
      write_int(f_out,s->sums[i].sum1);
      write_buf(f_out,s->sums[i].sum2,csum_length);
    }
  write_flush(f_out);
}


/*
  generate a stream of signatures/checksums that describe a buffer

  generate approximately one checksum every n bytes
  */
static struct sum_struct *generate_sums(struct map_struct *buf,off_t len,int n)
{
  int i;
  struct sum_struct *s;
  int count;
  int block_len = n;
  int remainder = (len%block_len);
  off_t offset = 0;

  count = (len+(block_len-1))/block_len;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("generate_sums");

  s->count = count;
  s->remainder = remainder;
  s->n = n;
  s->flength = len;

  if (count==0) {
    s->sums = NULL;
    return s;
  }

  if (verbose > 3)
    fprintf(FERROR,"count=%d rem=%d n=%d flength=%d\n",
	    s->count,s->remainder,s->n,(int)s->flength);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("generate_sums");
  
  for (i=0;i<count;i++) {
    int n1 = MIN(len,n);
    char *map = map_ptr(buf,offset,n1);

    s->sums[i].sum1 = get_checksum1(map,n1);
    get_checksum2(map,n1,s->sums[i].sum2);

    s->sums[i].offset = offset;
    s->sums[i].len = n1;
    s->sums[i].i = i;

    if (verbose > 3)
      fprintf(FERROR,"chunk[%d] offset=%d len=%d sum1=%08x\n",
	      i,(int)s->sums[i].offset,s->sums[i].len,s->sums[i].sum1);

    len -= n1;
    offset += n1;
  }

  return s;
}


/*
  receive the checksums for a buffer
  */
static struct sum_struct *receive_sums(int f)
{
  struct sum_struct *s;
  int i;
  off_t offset = 0;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("receive_sums");

  s->count = read_int(f);
  s->n = read_int(f);
  s->remainder = read_int(f);  
  s->sums = NULL;

  if (verbose > 3)
    fprintf(FERROR,"count=%d n=%d rem=%d\n",
	    s->count,s->n,s->remainder);

  if (s->count == 0) 
    return(s);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("receive_sums");

  for (i=0;i<s->count;i++) {
    s->sums[i].sum1 = read_int(f);
    read_buf(f,s->sums[i].sum2,csum_length);

    s->sums[i].offset = offset;
    s->sums[i].i = i;

    if (i == s->count-1 && s->remainder != 0) {
      s->sums[i].len = s->remainder;
    } else {
      s->sums[i].len = s->n;
    }
    offset += s->sums[i].len;

    if (verbose > 3)
      fprintf(FERROR,"chunk[%d] len=%d offset=%d sum1=%08x\n",
	      i,s->sums[i].len,(int)s->sums[i].offset,s->sums[i].sum1);
  }

  s->flength = offset;

  return s;
}


static int set_perms(char *fname,struct file_struct *file,struct stat *st,
		     int report)
{
  int updated = 0;
  struct stat st2;

  if (dry_run) return 0;

  if (!st) {
    if (link_stat(fname,&st2) != 0) {
      fprintf(FERROR,"stat %s : %s\n",fname,strerror(errno));
      return 0;
    }
    st = &st2;
  }

  if (preserve_times && !S_ISLNK(st->st_mode) &&
      st->st_mtime != file->modtime) {
    updated = 1;
    if (set_modtime(fname,file->modtime) != 0) {
      fprintf(FERROR,"failed to set times on %s : %s\n",
	      fname,strerror(errno));
      return 0;
    }
  }

#ifdef HAVE_CHMOD
  if (preserve_perms && !S_ISLNK(st->st_mode) &&
      st->st_mode != file->mode) {
    updated = 1;
    if (do_chmod(fname,file->mode) != 0) {
      fprintf(FERROR,"failed to set permissions on %s : %s\n",
	      fname,strerror(errno));
      return 0;
    }
  }
#endif

  if ((am_root && preserve_uid && st->st_uid != file->uid) || 
      (preserve_gid && st->st_gid != file->gid)) {
    updated = 1;
    if (do_lchown(fname,
	       (am_root&&preserve_uid)?file->uid:-1,
	       preserve_gid?file->gid:-1) != 0) {
      if (verbose>1 || preserve_uid)
	fprintf(FERROR,"chown %s : %s\n",fname,strerror(errno));
      return updated;
    }
  }
    
  if (verbose > 1 && report) {
    if (updated)
      fprintf(FINFO,"%s\n",fname);
    else
      fprintf(FINFO,"%s is uptodate\n",fname);
  }
  return updated;
}


/* choose whether to skip a particular file */
static int skip_file(char *fname,
		     struct file_struct *file, struct stat *st)
{
	if (st->st_size != file->length) {
		return 0;
	}
	
	/* if always checksum is set then we use the checksum instead 
	   of the file time to determine whether to sync */
	if (always_checksum && S_ISREG(st->st_mode)) {
		char sum[MD4_SUM_LENGTH];
		file_checksum(fname,sum,st->st_size);
		return (memcmp(sum,file->sum,csum_length) == 0);
	}

	if (ignore_times) {
		return 0;
	}

	return (st->st_mtime == file->modtime);
}


/* use a larger block size for really big files */
int adapt_block_size(struct file_struct *file, int bsize)
{
	int ret = file->length / (10000); /* rough heuristic */
	ret = ret & ~15; /* multiple of 16 */
	if (ret < bsize) ret = bsize;
	if (ret > CHUNK_SIZE/2) ret = CHUNK_SIZE/2;
	return ret;
}

void recv_generator(char *fname,struct file_list *flist,int i,int f_out)
{  
  int fd;
  struct stat st;
  struct map_struct *buf;
  struct sum_struct *s;
  int statret;
  struct file_struct *file = flist->files[i];

  if (verbose > 2)
    fprintf(FERROR,"recv_generator(%s,%d)\n",fname,i);

  statret = link_stat(fname,&st);

  if (S_ISDIR(file->mode)) {
    if (dry_run) return;
    if (statret == 0 && !S_ISDIR(st.st_mode)) {
      if (do_unlink(fname) != 0) {
	fprintf(FERROR,"unlink %s : %s\n",fname,strerror(errno));
	return;
      }
      statret = -1;
    }
    if (statret != 0 && mkdir(fname,file->mode) != 0 && errno != EEXIST) {
	    if (!(relative_paths && errno==ENOENT && 
		  create_directory_path(fname)==0 && 
		  mkdir(fname,file->mode)==0)) {
		    fprintf(FERROR,"mkdir %s : %s (2)\n",
			    fname,strerror(errno));
	    }
    }
    if (set_perms(fname,file,NULL,0) && verbose) 
      fprintf(FINFO,"%s/\n",fname);
    return;
  }

  if (preserve_links && S_ISLNK(file->mode)) {
#if SUPPORT_LINKS
    char lnk[MAXPATHLEN];
    int l;
    if (statret == 0) {
      l = readlink(fname,lnk,MAXPATHLEN-1);
      if (l > 0) {
	lnk[l] = 0;
	if (strcmp(lnk,file->link) == 0) {
	  set_perms(fname,file,&st,1);
	  return;
	}
      }
    }
    delete_file(fname);
    if (do_symlink(file->link,fname) != 0) {
      fprintf(FERROR,"link %s -> %s : %s\n",
	      fname,file->link,strerror(errno));
    } else {
      set_perms(fname,file,NULL,0);
      if (verbose) 
	fprintf(FINFO,"%s -> %s\n",
		fname,file->link);
    }
#endif
    return;
  }

#ifdef HAVE_MKNOD
  if (am_root && preserve_devices && IS_DEVICE(file->mode)) {
    if (statret != 0 || 
	st.st_mode != file->mode ||
	st.st_rdev != file->rdev) {	
      delete_file(fname);
      if (verbose > 2)
	fprintf(FERROR,"mknod(%s,0%o,0x%x)\n",
		fname,(int)file->mode,(int)file->rdev);
      if (do_mknod(fname,file->mode,file->rdev) != 0) {
	fprintf(FERROR,"mknod %s : %s\n",fname,strerror(errno));
      } else {
	set_perms(fname,file,NULL,0);
	if (verbose)
	  fprintf(FINFO,"%s\n",fname);
      }
    } else {
      set_perms(fname,file,&st,1);
    }
    return;
  }
#endif

  if (preserve_hard_links && check_hard_link(file)) {
    if (verbose > 1)
      fprintf(FINFO,"%s is a hard link\n",f_name(file));
    return;
  }

  if (!S_ISREG(file->mode)) {
    fprintf(FERROR,"skipping non-regular file %s\n",fname);
    return;
  }

  if (statret == -1) {
    if (errno == ENOENT) {
      write_int(f_out,i);
      if (!dry_run) send_sums(NULL,f_out);
    } else {
      if (verbose > 1)
	fprintf(FERROR,"recv_generator failed to open %s\n",fname);
    }
    return;
  }

  if (!S_ISREG(st.st_mode)) {
    if (delete_file(fname) != 0) {
      return;
    }

    /* now pretend the file didn't exist */
    write_int(f_out,i);
    if (!dry_run) send_sums(NULL,f_out);    
    return;
  }

  if (update_only && st.st_mtime > file->modtime) {
    if (verbose > 1)
      fprintf(FERROR,"%s is newer\n",fname);
    return;
  }

  if (skip_file(fname, file, &st)) {
    set_perms(fname,file,&st,1);
    return;
  }

  if (dry_run) {
    write_int(f_out,i);
    return;
  }

  if (whole_file) {
    write_int(f_out,i);
    send_sums(NULL,f_out);    
    return;
  }

  /* open the file */  
  fd = open(fname,O_RDONLY);

  if (fd == -1) {
    fprintf(FERROR,"failed to open %s : %s\n",fname,strerror(errno));
    fprintf(FERROR,"skipping %s\n",fname);
    return;
  }

  if (st.st_size > 0) {
    buf = map_file(fd,st.st_size);
  } else {
    buf = NULL;
  }

  if (verbose > 3)
    fprintf(FERROR,"gen mapped %s of size %d\n",fname,(int)st.st_size);

  s = generate_sums(buf,st.st_size,adapt_block_size(file, block_size));

  if (verbose > 2)
    fprintf(FERROR,"sending sums for %d\n",i);

  write_int(f_out,i);
  send_sums(s,f_out);
  write_flush(f_out);

  close(fd);
  if (buf) unmap_file(buf);

  free_sums(s);
}



static int receive_data(int f_in,struct map_struct *buf,int fd,char *fname)
{
  int i,n,remainder,len,count;
  off_t offset = 0;
  off_t offset2;
  char *data;
  static char file_sum1[MD4_SUM_LENGTH];
  static char file_sum2[MD4_SUM_LENGTH];
  char *map=NULL;

  count = read_int(f_in);
  n = read_int(f_in);
  remainder = read_int(f_in);

  sum_init();

  for (i=recv_token(f_in,&data); i != 0; i=recv_token(f_in,&data)) {
    if (i > 0) {
      if (verbose > 3)
	fprintf(FERROR,"data recv %d at %d\n",i,(int)offset);

      sum_update(data,i);

      if (fd != -1 && write_sparse(fd,data,i) != i) {
	fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
	exit_cleanup(1);
      }
      offset += i;
    } else {
      i = -(i+1);
      offset2 = i*n;
      len = n;
      if (i == count-1 && remainder != 0)
	len = remainder;

      if (verbose > 3)
	fprintf(FERROR,"chunk[%d] of size %d at %d offset=%d\n",
		i,len,(int)offset2,(int)offset);

      map = map_ptr(buf,offset2,len);

      see_token(map, len);
      sum_update(map,len);

      if (fd != -1 && write_sparse(fd,map,len) != len) {
	fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
	exit_cleanup(1);
      }
      offset += len;
    }
  }

  if (fd != -1 && offset > 0 && sparse_end(fd) != 0) {
    fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
    exit_cleanup(1);
  }

  sum_end(file_sum1);

  if (remote_version >= 14) {
    read_buf(f_in,file_sum2,MD4_SUM_LENGTH);
    if (verbose > 2)
      fprintf(FERROR,"got file_sum\n");
    if (fd != -1 && memcmp(file_sum1,file_sum2,MD4_SUM_LENGTH) != 0)
      return 0;
  }
  return 1;
}


static void delete_one(struct file_struct *f)
{
  if (!S_ISDIR(f->mode)) {
    if (do_unlink(f_name(f)) != 0) {
      fprintf(FERROR,"unlink %s : %s\n",f_name(f),strerror(errno));
    } else if (verbose) {
      fprintf(FERROR,"deleting %s\n",f_name(f));
    }
  } else {    
    if (do_rmdir(f_name(f)) != 0) {
      if (errno != ENOTEMPTY)
	fprintf(FERROR,"rmdir %s : %s\n",f_name(f),strerror(errno));
    } else if (verbose) {
      fprintf(FERROR,"deleting directory %s\n",f_name(f));      
    }
  }
}


/* yuck! This function wouldn't have been necessary if I had the sorting
   algorithm right. Unfortunately fixing the sorting algorithm would introduce
   a backward incompatibility as file list indexes are sent over the link.

   The aim is to see if a directory has already had the deletion algorithm applied
   to it (due to recursion), and if so to skip it. The bisection is to 
   prevent this being an n^2 algorithm */
static int delete_already_done(struct file_list *flist,int j)
{
	int low=0,high=j-1;
	char *name;
	char *p;

	if (j == 0) return 0;

	name = strdup(f_name(flist->files[j]));

	if (!name) {
		fprintf(FERROR,"out of memory in delete_already_done");
		exit_cleanup(1);
	}

	p = strrchr(name,'/');
	if (!p) {
		free(name);
		return 0;
	}
	*p = 0;

	while (low != high) {
		int mid = (low+high)/2;
		int ret = strcmp(f_name(flist->files[flist_up(flist, mid)]),name);
		if (ret == 0) {
			free(name);
			return 1;
		}
		if (ret > 0) {
			high=mid;
		} else {
			low=mid+1;
		}
	}

	low = flist_up(flist, low);

	if (strcmp(f_name(flist->files[low]),name) == 0) {
		free(name);
		return 1;
	}

	free(name);
	return 0;
}


/* this deletes any files on the receiving side that are not present
   on the sending side. For version 1.6.4 I have changed the behaviour
   to match more closely what most people seem to expect of this option */
static void delete_files(struct file_list *flist)
{
  struct file_list *local_file_list;
  int i, j;

  if (cvs_exclude)
    add_cvs_excludes();

  for (j=0;j<flist->count;j++) {
	  char *name = f_name(flist->files[j]);

	  if (!S_ISDIR(flist->files[j]->mode)) continue;

	  if (delete_already_done(flist, j)) continue;

	  if (!(local_file_list = send_file_list(-1,1,&name)))
		  continue;

	  if (verbose > 1)
		  fprintf(FINFO,"deleting in %s\n", name);

	  for (i=local_file_list->count-1;i>=0;i--) {
		  if (!local_file_list->files[i]->basename) continue;
		  if (-1 == flist_find(flist,local_file_list->files[i])) {
			  delete_one(local_file_list->files[i]);
		  }    
	  }
	  flist_free(local_file_list);
  }
}

static char *cleanup_fname;

void exit_cleanup(int code)
{
	if (cleanup_fname)
		do_unlink(cleanup_fname);
	signal(SIGUSR1, SIG_IGN);
	if (code) {
		kill_all(SIGUSR1);
	}
	exit(code);
}

void sig_int(void)
{
  exit_cleanup(1);
}


int recv_files(int f_in,struct file_list *flist,char *local_name,int f_gen)
{  
  int fd1,fd2;
  struct stat st;
  char *fname;
  char fnametmp[MAXPATHLEN];
  struct map_struct *buf;
  int i;
  struct file_struct *file;
  int phase=0;
  int recv_ok;

  if (verbose > 2) {
    fprintf(FERROR,"recv_files(%d) starting\n",flist->count);
  }

  if (recurse && delete_mode && !local_name && flist->count>0) {
    delete_files(flist);
  }

  while (1) 
    {      
      i = read_int(f_in);
      if (i == -1) {
	if (phase==0 && remote_version >= 13) {
	  phase++;
	  csum_length = SUM_LENGTH;
	  if (verbose > 2)
	    fprintf(FERROR,"recv_files phase=%d\n",phase);
	  write_int(f_gen,-1);
	  write_flush(f_gen);
	  continue;
	}
	break;
      }

      file = flist->files[i];
      fname = f_name(file);

      if (local_name)
	fname = local_name;

      if (dry_run) {
	if (!am_server && verbose)
	  printf("%s\n",fname);
	continue;
      }

      if (verbose > 2)
	fprintf(FERROR,"recv_files(%s)\n",fname);

      /* open the file */  
      fd1 = open(fname,O_RDONLY);

      if (fd1 != -1 && fstat(fd1,&st) != 0) {
	fprintf(FERROR,"fstat %s : %s\n",fname,strerror(errno));
	receive_data(f_in,NULL,-1,NULL);
	close(fd1);
	continue;
      }

      if (fd1 != -1 && !S_ISREG(st.st_mode)) {
	fprintf(FERROR,"%s : not a regular file (recv_files)\n",fname);
	receive_data(f_in,NULL,-1,NULL);
	close(fd1);
	continue;
      }

      if (fd1 != -1 && st.st_size > 0) {
	buf = map_file(fd1,st.st_size);
	if (verbose > 2)
	  fprintf(FERROR,"recv mapped %s of size %d\n",fname,(int)st.st_size);
      } else {
	buf = NULL;
      }

      /* open tmp file */
      if (strlen(fname) > (MAXPATHLEN-8)) {
	fprintf(FERROR,"filename too long\n");
	if (buf) unmap_file(buf);
	close(fd1);
	continue;
      }
      if (tmpdir) {
	      char *f;
	      f = strrchr(fname,'/');
	      if (f == NULL) 
		      f = fname;
	      else 
		      f++;
	      sprintf(fnametmp,"%s/%s.XXXXXX",tmpdir,f);
      } else {
	      sprintf(fnametmp,"%s.XXXXXX",fname);
      }
      if (NULL == mktemp(fnametmp)) {
	fprintf(FERROR,"mktemp %s failed\n",fnametmp);
	receive_data(f_in,buf,-1,NULL);
	if (buf) unmap_file(buf);
	close(fd1);
	continue;
      }
      fd2 = do_open(fnametmp,O_WRONLY|O_CREAT|O_EXCL,file->mode);
      if (fd2 == -1 && relative_paths && errno == ENOENT && 
	  create_directory_path(fnametmp) == 0) {
	      fd2 = do_open(fnametmp,O_WRONLY|O_CREAT|O_EXCL,file->mode);
      }
      if (fd2 == -1) {
	fprintf(FERROR,"open %s : %s\n",fnametmp,strerror(errno));
	receive_data(f_in,buf,-1,NULL);
	if (buf) unmap_file(buf);
	close(fd1);
	continue;
      }
      
      cleanup_fname = fnametmp;

      if (!am_server && verbose)
	printf("%s\n",fname);

      /* recv file data */
      recv_ok = receive_data(f_in,buf,fd2,fname);

      if (buf) unmap_file(buf);
      if (fd1 != -1) {
	close(fd1);
      }
      close(fd2);

      if (verbose > 2)
	fprintf(FERROR,"renaming %s to %s\n",fnametmp,fname);

      if (make_backups) {
	char fnamebak[MAXPATHLEN];
	if (strlen(fname) + strlen(backup_suffix) > (MAXPATHLEN-1)) {
		fprintf(FERROR,"backup filename too long\n");
		continue;
	}
	sprintf(fnamebak,"%s%s",fname,backup_suffix);
	if (rename(fname,fnamebak) != 0 && errno != ENOENT) {
	  fprintf(FERROR,"rename %s %s : %s\n",fname,fnamebak,strerror(errno));
	  continue;
	}
      }

      /* move tmp file over real file */
      if (rename(fnametmp,fname) != 0) {
	      if (errno == EXDEV) {
		      /* rename failed on cross-filesystem link.  
			 Copy the file instead. */
		      if (copy_file(fnametmp,fname, file->mode)) {
			      fprintf(FERROR,"copy %s -> %s : %s\n",
				      fnametmp,fname,strerror(errno));
		      } else {
			      set_perms(fname,file,NULL,0);
		      }
		      do_unlink(fnametmp);
	      } else {
		      fprintf(FERROR,"rename %s -> %s : %s\n",
			      fnametmp,fname,strerror(errno));
		      do_unlink(fnametmp);
	      }
      } else {
	      set_perms(fname,file,NULL,0);
      }

      cleanup_fname = NULL;


      if (!recv_ok) {
	      if (csum_length == SUM_LENGTH) {
		      fprintf(FERROR,"ERROR: file corruption in %s. File changed during transfer?\n",
			      fname);
	      } else {
		      if (verbose > 1)
			      fprintf(FERROR,"redoing %s(%d)\n",fname,i);
		      write_int(f_gen,i);
	      }
      }
    }

  if (preserve_hard_links)
	  do_hard_links(flist);

  /* now we need to fix any directory permissions that were 
     modified during the transfer */
  for (i = 0; i < flist->count; i++) {
	  struct file_struct *file = flist->files[i];
	  if (!file->basename || !S_ISDIR(file->mode)) continue;
	  recv_generator(f_name(file),flist,i,-1);
  }

  if (verbose > 2)
    fprintf(FERROR,"recv_files finished\n");
  
  return 0;
}



void send_files(struct file_list *flist,int f_out,int f_in)
{ 
  int fd;
  struct sum_struct *s;
  struct map_struct *buf;
  struct stat st;
  char fname[MAXPATHLEN];  
  int i;
  struct file_struct *file;
  int phase = 0;
  int offset=0;

  if (verbose > 2)
    fprintf(FERROR,"send_files starting\n");

  setup_nonblocking(f_in,f_out);

  while (1) {
	  i = read_int(f_in);
	  if (i == -1) {
		  if (phase==0 && remote_version >= 13) {
			  phase++;
			  csum_length = SUM_LENGTH;
			  write_int(f_out,-1);
			  write_flush(f_out);
			  if (verbose > 2)
				  fprintf(FERROR,"send_files phase=%d\n",phase);
			  continue;
		  }
		  break;
	  }

	  file = flist->files[i];

	  fname[0] = 0;
	  if (file->basedir) {
		  strncpy(fname,file->basedir,MAXPATHLEN-1);
		  fname[MAXPATHLEN-1] = 0;
		  if (strlen(fname) == MAXPATHLEN-1) {
			  fprintf(FERROR, "send_files failed on long-named directory %s\n",
				  fname);
			  return;
		  }
		  strcat(fname,"/");
		  offset = strlen(file->basedir)+1;
	  }
	  strncat(fname,f_name(file),MAXPATHLEN-strlen(fname));
	  
	  if (verbose > 2) 
		  fprintf(FERROR,"send_files(%d,%s)\n",i,fname);
	  
	  if (dry_run) {	
		  if (!am_server && verbose)
			  printf("%s\n",fname);
		  write_int(f_out,i);
		  continue;
	  }

	  s = receive_sums(f_in);
	  if (!s) {
		  fprintf(FERROR,"receive_sums failed\n");
		  return;
	  }
	  
	  fd = open(fname,O_RDONLY);
	  if (fd == -1) {
		  fprintf(FERROR,"send_files failed to open %s: %s\n",
			  fname,strerror(errno));
		  free_sums(s);
		  continue;
	  }
	  
	  /* map the local file */
	  if (fstat(fd,&st) != 0) {
		  fprintf(FERROR,"fstat failed : %s\n",strerror(errno));
		  free_sums(s);
		  close(fd);
		  return;
	  }
	  
	  if (st.st_size > 0) {
		  buf = map_file(fd,st.st_size);
	  } else {
		  buf = NULL;
	  }
	  
	  if (verbose > 2)
		  fprintf(FERROR,"send_files mapped %s of size %d\n",
			  fname,(int)st.st_size);
	  
	  write_int(f_out,i);
	  
	  write_int(f_out,s->count);
	  write_int(f_out,s->n);
	  write_int(f_out,s->remainder);
	  
	  if (verbose > 2)
		  fprintf(FERROR,"calling match_sums %s\n",fname);
	  
	  if (!am_server && verbose)
		  printf("%s\n",fname+offset);
	  
	  match_sums(f_out,s,buf,st.st_size);
	  write_flush(f_out);
	  
	  if (buf) unmap_file(buf);
	  close(fd);
	  
	  free_sums(s);
	  
	  if (verbose > 2)
		  fprintf(FERROR,"sender finished %s\n",fname);
  }

  if (verbose > 2)
	  fprintf(FERROR,"send files finished\n");

  match_report();

  write_int(f_out,-1);
  write_flush(f_out);
}



void generate_files(int f,struct file_list *flist,char *local_name,int f_recv)
{
  int i;
  int phase=0;

  if (verbose > 2)
    fprintf(FERROR,"generator starting pid=%d count=%d\n",
	    (int)getpid(),flist->count);

  for (i = 0; i < flist->count; i++) {
    struct file_struct *file = flist->files[i];
    mode_t saved_mode = file->mode;
    if (!file->basename) continue;

    /* we need to ensure that any directories we create have writeable
       permissions initially so that we can create the files within
       them. This is then fixed after the files are transferred */
    if (!am_root && S_ISDIR(file->mode)) {
      file->mode |= S_IWUSR; /* user write */
    }

    recv_generator(local_name?local_name:f_name(file),
		   flist,i,f);

    file->mode = saved_mode;
  }

  phase++;
  csum_length = SUM_LENGTH;
  ignore_times=1;

  if (verbose > 2)
    fprintf(FERROR,"generate_files phase=%d\n",phase);

  write_int(f,-1);
  write_flush(f);

  if (remote_version >= 13) {
    /* in newer versions of the protocol the files can cycle through
       the system more than once to catch initial checksum errors */
    for (i=read_int(f_recv); i != -1; i=read_int(f_recv)) {
      struct file_struct *file = flist->files[i];
      recv_generator(local_name?local_name:f_name(file),
		     flist,i,f);    
    }

    phase++;
    if (verbose > 2)
      fprintf(FERROR,"generate_files phase=%d\n",phase);

    write_int(f,-1);
    write_flush(f);
  }


  if (verbose > 2)
    fprintf(FERROR,"generator wrote %ld\n",(long)write_total());
}


