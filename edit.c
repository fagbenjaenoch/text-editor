#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define EDIT_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
{
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** data ***/

typedef struct erow
{
	int size;
	char *chars;
} erow;

struct editorConfig
{
	int cx, cy;
	int rowoff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

/**
 * Error handling function that cleans up the terminal and exits
 * Called when fatal errors occur (file operations, terminal operations)
 * Clears screen, resets cursor position, prints error message and exits
 */
void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

/**
 * Restores the terminal to its original settings when the editor exits
 * Called automatically at exit via atexit()
 * Reverts all changes made by enableRawMode()
 */
void disableRawMode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcseattr");
}

/**
 * Configures terminal for raw mode operation
 * - Disables terminal echo
 * - Disables canonical mode (line buffering)
 * - Disables Ctrl-C and Ctrl-Z signals
 * - Disables Ctrl-S and Ctrl-Q flow control
 * - Configures read timeout settings
 */
void enableRawMode()
{
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	// turning off ICANON makes the terminal read character by character instead of the default line by line
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

/**
 * Reads a keypress from the terminal
 * Handles escape sequences for special keys (arrows, home, end, etc)
 * Returns either a single character or a special key code from editorKey enum
 * Blocks until a key is read
 */
int editorReadKey()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	if (c == '\x1b')
	{
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';
		if (seq[0] == '[')
		{
			if (seq[1] >= '0' && seq[1] <= '9')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~')
				{
					switch (seq[1])
					{
					case '1':
						return HOME_KEY;
					case '3':
						return DEL_KEY;
					case '4':
						return END_KEY;
					case '5':
						return PAGE_UP;
					case '6':
						return PAGE_DOWN;
					case '7':
						return HOME_KEY;
					case '8':
						return END_KEY;
					}
				}
			}
			else
			{
				switch (seq[1])
				{
				case 'A':
					return ARROW_UP;
				case 'B':
					return ARROW_DOWN;
				case 'C':
					return ARROW_RIGHT;
				case 'D':
					return ARROW_LEFT;
				case 'H':
					return HOME_KEY;
				case 'F':
					return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O')
		{
			switch (seq[1])
			{
			case 'H':
				return HOME_KEY;
			case 'F':
				return END_KEY;
			}
		}
		return '\x1b';
	}
	else
	{
		return c;
	}
}

int getCursorPosition(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	while (i < sizeof(buf) - 1)
	{
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	}
	else
	{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

/**
 * Adds a new row of text to the editor's buffer
 * @param s: The string to add
 * @param len: Length of the string
 * Dynamically allocates memory for the new row
 * Updates the editor's row count
 */
void editorAppendRow(char *s, size_t len)
{
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.numrows++;
}

/*** file i/o ***/

/**
 * Opens and reads a file into the editor buffer
 * @param filename: Path to the file to open
 * Reads the file line by line
 * Strips newline characters
 * Adds each line to the editor's row buffer
 */
void editorOpen(char *filename)
{
	FILE *fp = fopen(filename, "r");
	if (!fp)
		die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = getline(&line, &linecap, fp)) != -1)
	{
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;

		editorAppendRow(line, linelen);
	}

	free(line);
	fclose(fp);
}

/*** append buffer ***/

struct abuf
{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

/**
 * Append buffer: Efficient string building for terminal output
 * @param ab: Append buffer structure
 * @param s: String to append
 * @param len: Length of string
 * Dynamically grows the buffer as needed
 * Used to build the complete screen output before writing to terminal
 */
void abAppend(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL)
		return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab)
{
	free(ab->b);
}

/*** output ***/

/**
 * Handles vertical scrolling of the editor window
 * Updates E.rowoff (row offset) based on cursor position
 * Ensures cursor stays within visible portion of the screen
 * Called before each screen refresh
 */
void editorScroll()
{
	if (E.cy < E.rowoff)
	{
		E.rowoff = E.cy;
	}

	if (E.cy >= E.rowoff + E.screenrows)
	{
		E.rowoff = E.cy - E.screenrows + 1;
	}
}

/**
 * Renders each row of the editor
 * @param ab: Append buffer for building output
 * Handles:
 * - Drawing file contents
 * - Welcome message when buffer is empty
 * - Tilde markers for lines past end of file
 * - Truncating lines that exceed screen width
 */
void editorDrawRows(struct abuf *ab)
{
	int y;
	for (y = 0; y < E.screenrows; y++)
	{
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows)
		{
			if (E.numrows == 0 && y == E.screenrows / 3)
			{
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome),
																	"termEdit editor -- version %s", EDIT_VERSION);
				if (welcomelen > E.screencols)
					welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding)
				{
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--)
					abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			}
			else
			{
				abAppend(ab, "~", 1);
			}
		}
		else
		{
			int len = E.row[filerow].size;
			// You'd find a lot of the checks below, it is used to truncate the row if it is greater than the terminal column size
			if (len > E.screencols)
				len = E.screencols;
			abAppend(ab, E.row[filerow].chars, len);
		}
		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1)
		{
			abAppend(ab, "\r\n", 2);
		}
	}
}
/**
 * Main screen refresh function
 * Handles complete redraw of the editor screen
 * - Updates scroll position
 * - Hides cursor during redraw
 * - Clears screen and redraws all rows
 * - Positions cursor
 * - Shows cursor again
 * Uses append buffer for efficient terminal I/O
 */
void editorRefreshScreen()
{
	editorScroll();
	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/

/**
 * Handles cursor movement based on arrow key input
 * @param key: The key pressed (ARROW_LEFT, ARROW_RIGHT, etc)
 * Updates cursor position (E.cx, E.cy)
 * Implements bounds checking to prevent cursor from going off-screen
 */
void editorMoveCursor(int key)
{
	switch (key)
	{
	case ARROW_LEFT:
		if (E.cx != 0)
		{
			E.cx--;
		}
		break;
	case ARROW_RIGHT:
		if (E.cy != E.screencols - 1)
		{
			E.cx++;
		}
		break;
	case ARROW_UP:
		if (E.cy != 0)
		{
			E.cy--;
		}
		break;
	case ARROW_DOWN:
		if (E.cx < E.numrows)
		{
			E.cy++;
		}
		break;

	default:
		break;
	}
}

/**
 * Main input handling function
 * Reads and processes each keypress
 * Handles:
 * - Editor commands (Ctrl-Q to quit)
 * - Cursor movement (arrows)
 * - Page up/down
 * - Home/End keys
 * Main input loop of the editor
 */
void editorProcessKeypress()
{
	int c = editorReadKey();

	switch (c)
	{
	case CTRL_KEY('q'):
		write(STDOUT_FILENO, "\x1b[2J", 4);
		write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);
		break;

	case HOME_KEY:
		E.cx = 0;
		break;
	case END_KEY:
		E.cx = E.screencols - 1;
		break;

	case PAGE_DOWN:
	case PAGE_UP:
	{
		int times = E.screenrows;
		while (times--)
		{
			editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		}
	}
	break;

	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editorMoveCursor(c);
		break;
	}
}

/*** init ***/

/**
 * Initializes the editor state
 * - Resets cursor position
 * - Initializes row offset
 * - Gets terminal window size
 * - Initializes empty text buffer
 * Called once at program start
 */
void initEditor()
{
	E.cx = 0;
	E.cy = 0;
	E.rowoff = 0;
	E.numrows = 0;
	E.row = NULL;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if (argc >= 2)
	{
		editorOpen(argv[1]);
	}

	while (1)
	{
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
