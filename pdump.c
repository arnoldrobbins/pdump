/*
*	pdump --- read Prime format MAGSAV tapes like we do 'tar' tapes
*
*	Author: Jeff Lee, with some help from Arnold Robbins
*/

#include <stdio.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <vaxmba/htreg.h>
#include <vaxmba/mtreg.h>
#include <vaxuba/tmreg.h>
#include <vaxuba/tsreg.h>
#include <vaxuba/utreg.h>
#include <sys/mtio.h>

/*
*	global definitions and variables used in ptar
*/

/*
*	TAPESIZE --- size of the name of a tape in a magsav header
*	DATESIZE --- size of the date in the header of a magsav tape
*	FILESIZE --- maximum filename size
*	MAXNAMES --- maximum number of files that can be loaded
*	MAXBLOCK --- maximum size of a physical tape block
*/

#define TAPESIZE 6
#define DATESIZE 6
#define FILESIZE 32
#define MAXNAMES 1024
#define MAXBLOCK 4096

/*
*	Lastseq --- the last sequence number we read from the tape
*	State --- the current state of our finite state machine
*	Size --- the size of the current working buffer
*	Reel --- the current reel of tape we are reading
*	Name --- name of the tape we are reading
*	Date --- date of the tape we are reading
*	File --- name of the current working file
*	Accept --- set if a path matches a given file name
*	Savfile --- saved file name for changing modes, etc.
*	Savcnt --- saved count to index where to get access times
*	Names --- list of names to be loaded
*	Buf --- current working tape buffer
*	Rev --- revision number of the tape
*	Dc1 --- blank compression flag
*	Td --- current descriptor for the tape
*	Fd --- current descriptor for the file
*	Tflag --- table of contents only, do not dump files
*	Vflag --- verbose, print file names as we go
*	Xflag --- extract, this is the default
*	Pflag --- restore protections
*/

int Lastseq;
int Accept;
int State;
int Size;
int Reel;
int Rev;
int Dc1;
int Td;
int Fd;

int Savcnt;
char Name[TAPESIZE];
char Date[DATESIZE];
char File[MAXBLOCK];
char Savfile[MAXBLOCK];

struct
{
	char *name;
	int namlen;
	int state;
}	Names[MAXNAMES]		= { { "", 0, 2 } };

int Tflag = 0;
int Vflag = 0;
int Xflag = 1;
int Pflag = 0;

/*
*	head_type --- data structure for reading the tape header
*/

typedef struct
{
	unsigned char h_seqnum[2];
	unsigned char h_length[2];
	unsigned char h_id[2];
	unsigned char h_class[2];
	char h_date[6];
	unsigned char h_rev[2];
	unsigned char h_reel[2];
	char h_name[6];
}	head_type;

/*
*	macros to pull the date out of the date_modified field
*/

#define Year(date)	((date >> 9) & 0177)	/* 19(00) - 19(127) */
#define Month(date)	((date >> 5) & 017)	/* Jan(1) - Dec(12) */
#define Day(date)	(date & 037)		/* 1 -31 */

/*
*	macros to get the protection info
*/

#define Odelete(prot)	(prot & 02000)		/* owner can delete */
#define Owrite(prot)	(prot & 01000)		/* owner can write */
#define Oread(prot)	(prot & 00400)		/* owner can read */
#define Delprot(prot)	(prot & 00200)		/* prevent deletion */
#define Ndelete(prot)	(prot & 00004)		/* nonowner can delete */
#define Nwrite(prot)	(prot & 00002)		/* nonowner can write */
#define Nread(prot)	(prot & 00001)		/* nonowner can read */

/*
*	tree_type --- structure returned for each treename
*/

typedef struct
{
	unsigned char t_type;
	unsigned char t_unused1[1];
	char t_name[32];
	unsigned char t_prot[2];
	unsigned char t_aclprot[2];
	unsigned char t_unused2[1];
	unsigned char t_file;
	unsigned char t_date[2];		/* last date of modification */
	unsigned char t_time[2];		/* last time of modification */
	unsigned char t_unused3[4];
}	tree_type;

/*
*	dir_type --- data structure for extracting pathnames
*/

typedef struct
{
	unsigned char d_seqnum[2];
	unsigned char d_length[2];
	unsigned char d_rectyp[2];
	tree_type d_tree[1];
}	dir_type;

/*
*	data_type --- descriptor for a purely data record
*/

typedef struct
{
	unsigned char d_seqnum[2];
	unsigned char d_length[2];
	unsigned char d_rectyp[2];
	char d_data[MAXBLOCK];
}	data_type;

/*
*	Buf --- global buffer that everyone reads from
*/

union
{
	head_type head;
	data_type data;
	dir_type dir;
}	Buf;

main(argc, argv)
int argc;
char **argv;
{

/*
*	crack the arguments, setup the file descriptor, and initialize
*	the state machine.
*/
	initialize(argc, argv);

/*
*	change from state to state depending on the contents of the tape
*/

	state();

/*
*	cleanup and terminate
*/

	finish();
}


/*
*	initialize --- crack arguments, set file descriptor, and initialize
*		the state machine.
*/

initialize(argc, argv)
int argc;
char **argv;
{
	char *input_device = "/dev/rmt12";	/* default device */
	int i, ap = 1;

	if (argv[1][0] == '-')
	{
		ap++;
		for(i = 1; argv[1][i] != '\0'; i++)
			switch (argv[1][i]) {
			case 't':
			case 'T':
				Tflag = 1;
				break;

			case 'v':
			case 'V':
				Vflag = 1;
				break;
			
			case 'x':
			case 'X':
				Xflag = 1;
				break;
			
			case 'f':
			case 'F':
				ap++;
				if (argc < 3 || strncmp(argv[2], "/dev/", 5) != 0)
					usage();
				else
					input_device = argv[2];
				break;
			
			case 'p':
			case 'P':
				Pflag = 1;
				break;

			default:
				usage();
				break;
			}
	}

/*
*	collect the list of filenames to process
*/

	for (i = 0; argv[ap] != NULL; i++, ap++)
	{
		Names[i].name = argv[ap];
		Names[i].namlen = strlen(argv[ap]);
		Names[i].state = 2;
	}

/*
 *	Open the tape drive. This will either be the default device, or the
 *	the device specified on the command line.  Another possibility is to
 *	try to read a few words (12) to try to figure out the density by
 *	itself. If you do, remember to rewind it.
 */

	Td = open(input_device, 0);
	if (Td == -1)
	{
		perror("initialize");
		exit(1);
	}

/*
*	we've got to start at the beginning
*/

	State = 1;
}


/*
*	state --- move from state to state depending on our input
*/

state()
{


	while (State)
	{
		Size = read(Td, &Buf, MAXBLOCK);

		if (Size == -1)
		{
			check_eot();
			continue;
		}

		switch (State)
		{
			case 1:
				check1();
				break;
			case 2:
				check2();
				break;
			case 3:
				check3();
				break;
			case 4:
				check4();
				break;
			case 5:
				check5();
				break;
			case 6:
				check6();
				break;
			default:
				fprintf(stderr, "state: can't happen\n");
				exit(1);
		}
	}
}


/*
*	check1 --- ensure that we have a legal id record and go to state 2
*/

check1()
{
	int i;


/*
*	extract the ineresting information from the tape header
*/

	Lastseq = ((int) Buf.head.h_seqnum[0] << 8) + (int) Buf.head.h_seqnum[1];
	Reel = ((int) Buf.head.h_reel[0] << 8) + (int) Buf.head.h_reel[1];
	Rev = ((int) Buf.head.h_rev[0] << 8) + (int) Buf.head.h_rev[1];

/*
*	remove the name and date, remembering to reset the parity bits
*	before we stash them someplace
*/

	for (i = 0; i < DATESIZE; i++)
	{
		if ((Buf.head.h_date[i] & 0177) == ' ')
			break;

		Date[i] = Buf.head.h_date[i] & 0177;
	}
	Date[i] = '\0';

	for (i = 0; i < TAPESIZE; i++)
	{
		if ((Buf.head.h_name[i] & 0177) == ' ')
			break;

		Name[i] = Buf.head.h_name[i] & 0177;
	}
	Name[i] = '\0';

/*
*	set State to the next state and return
*/

printf("Tape '%s', Reel %d, Revision %d, Date %s\n", Name, Reel, Rev, Date);
	State = 2;
}


/*
*	check2 --- ensure that we have an eof and go to state 3
*/

check2()
{


	if (Size != 0)
	{
		fprintf(stderr, "check2: misformatted MAGSAV tape\n");
		exit(1);
	}

	State = 3;
}


/*
*	check4 --- check for a password record after a directory record
*/

check4()
{
	int i;


/*
*	check that we have a data record after the directory record
*/

	i = ((int) Buf.data.d_rectyp[0] << 8) + (int) Buf.data.d_rectyp[1];
	if (i != 1)
	{
		fprintf(stderr, "check4: record is not a password record\n");
		exit(1);
	}

/*
*	change state
*/

	State = 3;
}


/*
*	check3 --- determine if the current entry is a directory, a file,
*		or an end-of-tape record
*/

check3()
{
	char c;
	char *p;
	int i, j, k, cnt;

	char *rindex();


/*
*	first check for an eot record, since it is handled differently
*/

	i = ((int) Buf.data.d_rectyp[0] << 8) + (int) Buf.data.d_rectyp[1];
	if (i == 5)
	{
		State = 6;
		return;
	}

/*
*	should be either a file or directory entry. check first that it
*	is a treename-type record and if so, collect the name.
*/

	if (i != 2)
	{
		fprintf(stderr, "check3: illegal record type\n");
		exit(1);
	}

	cnt = ((int) Buf.dir.d_length[0] << 8) + (int) Buf.dir.d_length[1];
	cnt = (cnt - 3) / 24;

	for (i = 0, j = 0; i < cnt; i++)
	{
		for (k = 0; k < FILESIZE; k++)
		{
			c = Buf.dir.d_tree[i].t_name[k] & 0177;
			if (c == ' ')
				break;

			File[j++] = isupper(c) ? tolower(c) : c;
		}

		File[j++] = '/';
	}
	File[--j] = '\0';

/*
*	search the list of files and see if we have a prefix of this file.
*	also update the flag status and see if we have finished the tape.
*/

	k = 0;
	Accept = 0;
	for (i = 0; Names[i].name != NULL; i++)
	{
		if (!strncmp(Names[i].name, File, Names[i].namlen) &&
		   (!File[Names[i].namlen] || File[Names[i].namlen] == '/' ||
		    !Names[i].namlen))
		{
			Names[i].state = 1;
			if (!Accept)
			{
				Accept = 1;
				j = i;
			}
		}
		else if (Names[i].state == 1)
			Names[i].state = 0;

		if (Names[i].state)
			k = 1;
	}

	if (k == 0)
	{
		State = 0;
		return;
	}

	if (Accept && (p = rindex(Names[j].name, '/')) != NULL)
	{
		for (k = 0, p++; *p; k++, p++)
			File[k] = *p;

		for (i = Names[j].namlen; File[i];)
			File[k++] = File[i++];

		File[k] = '\0';
	}

/*
*	check the type of the file and set the state accordingly
*	also create the file or directory while we are at it
*/

	i = Buf.dir.d_tree[cnt-1].t_file;
	if (i == 0 || i == 1)
	{
		if (Fd)
			close(Fd);

		Dc1 = 0;
		if (! Tflag && Accept)
			Fd = creat(File, prot_mode(cnt-1));

		strcpy(Savfile, File);
		Savcnt = cnt - 1;

		State = 5;
	}
	else
	{
		if (! Tflag && (i == 4 || i == 5) && Accept)
			mkdir(File);

		State = 4;
	}

	if ((Vflag || Tflag) && Accept)
		printf("%s\n", File);
}


/*
*	check5 --- see if there is data to write or another file to create
*/

check5()
{
	char c;
	char *p;
	int i, j, k, cnt;

	char *rindex();


/*
*	check for an eot record first
*/

	i = ((int) Buf.data.d_rectyp[0] << 8) + (int) Buf.data.d_rectyp[1];
	if (i == 5)
	{
		State = 6;
		return;
	}

/*
*	simply check the record type for data or treename
*/

	i = ((int) Buf.data.d_rectyp[0] << 8) + (int) Buf.data.d_rectyp[1];
	if (i == 1)
	{
		if (Accept)
			copyout();

		State = 5;
		return;
	}
	else if (i != 2)
	{
		fprintf(stderr, "check5: illegal record type\n");
		exit(1);
	}

/*
*	calculate the number of entries and build the file name
*/

	cnt = ((int) Buf.dir.d_length[0] << 8) + (int) Buf.dir.d_length[1];
	cnt = (cnt - 3) / 24;

	for (i = 0, j = 0; i < cnt; i++)
	{
		for (k = 0; k < FILESIZE; k++)
		{
			c = Buf.dir.d_tree[i].t_name[k] & 0177;
			if (c == ' ')
				break;

			File[j++] = isupper(c) ? tolower(c) : c;
		}

		File[j++] = '/';
	}
	File[--j] = '\0';

/*
*	search the list of files and see if we have a prefix of this file.
*	also update the flag status and see if we have finished the tape.
*/

	k = 0;
	Accept = 0;
	for (i = 0; Names[i].name != NULL; i++)
	{
		if (!strncmp(Names[i].name, File, Names[i].namlen) &&
		   (!File[Names[i].namlen] || File[Names[i].namlen] == '/' ||
		    !Names[i].namlen))
		{
			Names[i].state = 1;
			if (!Accept)
			{
				Accept = 1;
				j = i;
			}
		}
		else if (Names[i].state == 1)
			Names[i].state = 0;

		if (Names[i].state)
			k = 1;
	}

	if (k == 0)
	{
		State = 0;
		return;
	}

	if (Accept && (p = rindex(Names[j].name, '/')) != NULL)
	{
		for (k = 0, p++; *p; k++, p++)
			File[k] = *p;

		for (i = Names[j].namlen; File[i];)
			File[k++] = File[i++];

		File[k] = '\0';
	}

/*
*	figure whether it was a file or directory and set the state
*/

	i = Buf.dir.d_tree[cnt-1].t_file;
	if (i == 0 || i == 1)
	{
		if (! Tflag && Accept)
		{
			if (Fd)
				close(Fd);

			Fd = creat(File, prot_mode(cnt-1));
			strcpy(Savfile, File);
			Savcnt = cnt - 1;
		}

		State = 5;
	}
	else
	{
		if (! Tflag && (i == 4 || i == 5) && Accept)
			mkdir(File);

		State = 4;
	}

	if ((Vflag || Tflag) && Accept)
		printf("%s\n", File);
}


/*
*	check6 --- hit an eot record, ensure that we have 2 eof's
*/

check6()
{
	int i;


/*
*	check for the eof's
*/

	if (Size != 0 || (Size = read(Td, &Buf, MAXBLOCK)) != 0)
	{
		fprintf(stderr, "check6: missing eof after eot record\n");
		exit(1);
	}

/*
*	set the state to the final state and return
*/

	State = 0;
}


/*
*	mkdir --- create a directory with as little hassle as possible
*/

mkdir(dir)
char *dir;
{
	int pid;


	if ((pid = vfork()) == 0)
	{
		execl("/bin/mkdir", "mkdir", dir, 0);
		execl("/usr/bin/mkdir", "mkdir", dir, 0);
		fprintf(stderr, "mkdir: can't make %s\n", dir);
		exit(1);
	}
	else
		while (wait(0) != pid);
}


/*
*	copyout --- decompress the blanks and write out the last data block
*/

copyout()
{
	char buf[MAXBLOCK];
	int i, j, k, cnt;

	if (Tflag)
		return;

/*
*	calculate the number of CHAR's in this last block
*/

	cnt = ((int) Buf.data.d_length[0] << 8) + (int) Buf.data.d_length[1];
	cnt = (2 * cnt) - 6;

/*
*	test and interpret each character in the the data block. Prime sets
*	the parity bit for each character so we must reset them.
*/

	for (i = 0, j = 0; i < cnt; i++)
	{
		Buf.data.d_data[i] &= 0177;

/*
*	check for blank compression and undo it
*/

		if (Dc1)
		{
			for (k = 0; k < (int) Buf.data.d_data[i] && j < MAXBLOCK; k++, j++)
				buf[j] = ' ';

			if (j == MAXBLOCK)	/* buffer filled up */
			{
				write(Fd, buf, MAXBLOCK);
				for (j = 0; k < (int) Buf.data.d_data[i]; k++, j++)
					buf[j] = ' ';
			}

			Dc1 = 0;
			continue;
		}
		else if (Buf.data.d_data[i] == 021)	/* ^q */
		{
			Dc1 = 1;
			continue;
		}

/*
*	blank compression undone; now copy the character through unchanged.
*	if it happens to be a newline, we must check if it fell on an even
*	boundary and skip the next character if it did.
*/

		buf[j] = Buf.data.d_data[i];

		if (buf[j] == '\n' && !(i % 2))
			i++;

		j++;

/*
*	see if the buffer is full. if so, flush it and reset the buffer ptr
*/

		if (j == MAXBLOCK)
		{
			write(Fd, buf, MAXBLOCK);
			j = 0;
		}
	}

/*
*	flush any remaining characters since we don't want them hanging until
*	next time.
*/

	write(Fd, buf, j);
}


/*
*	check_eot --- see if the current tape error was caused by eot
*/

check_eot()
{
	struct mtget mt_stat;
	struct mtop  mt_oper;
	int eot;


	ioctl(Td, MTIOCGET, &mt_stat);
	switch(mt_stat.mt_type)
	{
		case MT_ISTS:
			eot = mt_stat.mt_erreg & TS_EOT;
			break;
		case MT_ISMT:
			eot = mt_stat.mt_dsreg & MTDS_EOT;
			break;
		case MT_ISUT:
			eot = mt_stat.mt_dsreg & UTDS_EOT;
			break;
		case MT_ISHT:
			eot = mt_stat.mt_dsreg & HTDS_EOT;
			break;
		case MT_ISTM:
			eot = mt_stat.mt_erreg & TMER_EOT;
			break;
		default:
			fprintf(stderr, "check_eot: unknown tape drive type\n");
			exit(1);
	}

	if (!eot)
	{
		perror("check_eot");
		exit(1);
	}

	printf("please mount next tape (hit cr to continue): ");
	fflush (stdout);

	mt_oper.mt_count = 1;
	mt_oper.mt_op = MTREW;
	ioctl(Td, MTIOCTOP, &mt_oper);

	while (getchar() != '\n')
		;
}


usage()		/* print out how to use and die */
{
	fprintf(stderr, "Usage: pdump [-xvtf] [tape_device]\n");
	exit (1);
}


#define OWNREAD		0400
#define OWNWRITE	0200
#define GRPREAD		0040
#define GRPWRITE	0020
#define OTHREAD		0004
#define OTHWRITE	0002

int prot_mode(cnt)	/* return proper file protection mode */
int cnt;
{
	int mode;
	int pmode;

	if (! Pflag)
		return(0644);		/* default mode */

	pmode = ((int) Buf.dir.d_tree[cnt].t_prot[0] << 8)
		+ ((int) Buf.dir.d_tree[cnt].t_prot[1]);

	mode = 0;
	if (Oread(pmode))
		mode |= OWNREAD;
	
	if (Owrite(pmode))
		mode |= OWNWRITE;

	/* set group and other to Prime other */

	if (Nread(pmode))
		mode |= (GRPREAD | OTHREAD);
	
	if (Nwrite(pmode))
		mode |= (GRPWRITE | OTHWRITE);

	return(mode);
}


/*
*	finish --- cleanup and print a list of error messages
*/

finish()
{
	int i;


	for (i = 0; Names[i].name != NULL; i++)
		if (Names[i].state == 2)
			fprintf(stderr, "%s: not on tape\n", Names[i].name);
}
