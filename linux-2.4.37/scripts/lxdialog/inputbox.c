/*
 *  inputbox.c -- implements the input box
 *
 *  ORIGINAL AUTHOR: Savio Lam (lam836@cs.cuhk.hk)
 *  MODIFIED FOR LINUX KERNEL CONFIG BY: William Roadcap (roadcap@cfw.com)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "dialog.h"

unsigned char dialog_input_result[MAX_LEN + 1];

/*
 *  Print the termination buttons
 */
static void
print_buttons(WINDOW *dialog, int height, int width, int selected)
{
    int x = width / 2 - 11;
    int y = height - 2;

    print_button (dialog, "  Ok  ", y, x, selected==0);
    print_button (dialog, " Help ", y, x + 14, selected==1);

    wmove(dialog, y, x+1+14*selected);
    wrefresh(dialog);
}

/*
 * Display a dialog box for inputing a string
 */
int
dialog_inputbox (const char *title, const char *prompt, int height, int width,
		 const char *init)
{
    int i, x, y, box_y, box_x, box_width;
    int input_x = 0, scroll = 0, key = 0, button = -1;
    unsigned char *instr = dialog_input_result;
    WINDOW *dialog;

    /* center dialog box on screen */
    x = (COLS - width) / 2;
    y = (LINES - height) / 2;


    draw_shadow (stdscr, y, x, height, width);

    dialog = newwin (height, width, y, x);
    keypad (dialog, TRUE);

    draw_box (dialog, 0, 0, height, width, dialog_attr, border_attr);
    wattrset (dialog, border_attr);
    mvwaddch (dialog, height-3, 0, ACS_LTEE);
    for (i = 0; i < width - 2; i++)
	waddch (dialog, ACS_HLINE);
    wattrset (dialog, dialog_attr);
    waddch (dialog, ACS_RTEE);

    if (title != NULL && strlen(title) >= width-2 ) {
	/* truncate long title -- mec */
	char * title2 = malloc(width-2+1);
	memcpy( title2, title, width-2 );
	title2[width-2] = '\0';
	title = title2;
    }

    if (title != NULL) {
	wattrset (dialog, title_attr);
	mvwaddch (dialog, 0, (width - strlen(title))/2 - 1, ' ');
	waddstr (dialog, (char *)title);
	waddch (dialog, ' ');
    }

    wattrset (dialog, dialog_attr);
    print_autowrap (dialog, prompt, width - 2, 1, 3);

    /* Draw the input field box */
    box_width = width - 6;
    getyx (dialog, y, x);
    box_y = y + 2;
    box_x = (width - box_width) / 2;
    draw_box (dialog, y + 1, box_x - 1, 3, box_width + 2,
	      border_attr, dialog_attr);

    print_buttons(dialog, height, width, 0);

    /* Set up the initial value */
    wmove (dialog, box_y, box_x);
    wattrset (dialog, inputbox_attr);

    if (!init)
	instr[0] = '\0';
    else
	strcpy (instr, init);

    input_x = strlen (instr);

    if (input_x >= box_width) {
	scroll = input_x - box_width + 1;
	input_x = box_width - 1;
	for (i = 0; i < box_width - 1; i++)
	    waddch (dialog, instr[scroll + i]);
    } else
	waddstr (dialog, instr);

    wmove (dialog, box_y, box_x + input_x);

    wrefresh (dialog);

    while (key != ESC) {
	key = wgetch (dialog);

	if (button == -1) {	/* Input box selected */
	    switch (key) {
	    case TAB:
	    case KEY_UP:
	    case KEY_DOWN:
		break;
	    case KEY_LEFT:
		continue;
	    case KEY_RIGHT:
		continue;
	    case KEY_BACKSPACE:
	    case 127:
		if (input_x || scroll) {
		    wattrset (dialog, inputbox_attr);
		    if (!input_x) {
			scroll = scroll < box_width - 1 ?
			    0 : scroll - (box_width - 1);
			wmove (dialog, box_y, box_x);
			for (i = 0; i < box_width; i++)
			    waddch (dialog, instr[scroll + input_x + i] ?
				    instr[scroll + input_x + i] : ' ');
			input_x = strlen (instr) - scroll;
		    } else
			input_x--;
		    instr[scroll + input_x] = '\0';
		    mvwaddch (dialog, box_y, input_x + box_x, ' ');
		    wmove (dialog, box_y, input_x + box_x);
		    wrefresh (dialog);
		}
		continue;
	    default:
		if (key < 0x100 && isprint (key)) {
		    if (scroll + input_x < MAX_LEN) {
			wattrset (dialog, inputbox_attr);
			instr[scroll + input_x] = key;
			instr[scroll + input_x + 1] = '\0';
			if (input_x == box_width - 1) {
			    scroll++;
			    wmove (dialog, box_y, box_x);
			    for (i = 0; i < box_width - 1; i++)
				waddch (dialog, instr[scroll + i]);
			} else {
			    wmove (dialog, box_y, input_x++ + box_x);
			    waddch (dialog, key);
			}
			wrefresh (dialog);
		    } else
			flash ();	/* Alarm user about overflow */
		    continue;
		}
	    }
	}
	switch (key) {
	case 'O':
	case 'o':
	    //delwin (dialog);
	    return 0;
	case 'H':
	case 'h':
	    //delwin (dialog);
	    return 1;
	case KEY_UP:
	case KEY_LEFT:
	    switch (button) {
	    case -1:
		button = 1;	/* Indicates "Cancel" button is selected */
		print_buttons(dialog, height, width, 1);
		break;
	    case 0:
		button = -1;	/* Indicates input box is selected */
		print_buttons(dialog, height, width, 0);
		wmove (dialog, box_y, box_x + input_x);
		wrefresh (dialog);
		break;
	    case 1:
		button = 0;	/* Indicates "OK" button is selected */
		print_buttons(dialog, height, width, 0);
		break;
	    }
	    break;
	case TAB:
	case KEY_DOWN:
	case KEY_RIGHT:
	    switch (button) {
	    case -1:
		button = 0;	/* Indicates "OK" button is selected */
		print_buttons(dialog, height, width, 0);
		break;
	    case 0:
		button = 1;	/* Indicates "Cancel" button is selected */
		print_buttons(dialog, height, width, 1);
		break;
	    case 1:
		button = -1;	/* Indicates input box is selected */
		print_buttons(dialog, height, width, 0);
		wmove (dialog, box_y, box_x + input_x);
		wrefresh (dialog);
		break;
	    }
	    break;
	case ' ':
	case '\n':
	    //delwin (dialog);
	    return (button == -1 ? 0 : button);
	case 'X':
	case 'x':
	    key = ESC;
	case ESC:
	    break;
	}
    }

    //delwin (dialog);
    return -1;			/* ESC pressed */
}
