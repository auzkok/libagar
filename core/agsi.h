/*	Public domain	*/
/*
 * Common ANSI escape sequences.
 * https://en.wikipedia.org/wiki/ANSI_escape_code
 */
#ifdef AG_ANSI_COLOR

# define AGSI_RST	"\x1b[0m"
# define AGSI_BOLD     	"\x1b[1m"
# define AGSI_FAINT    	"\x1b[2m"
# define AGSI_ITALIC   	"\x1b[3m"
# define AGSI_UNDERLINE	"\x1b[4m"
# define AGSI_REVERSE  	"\x1b[7m"
# define AGSI_BLK	"\x1b[30m"
# define AGSI_RED	"\x1b[31m"
# define AGSI_GRN	"\x1b[32m"
# define AGSI_YEL	"\x1b[33m"
# define AGSI_BLU	"\x1b[34m"
# define AGSI_MAG	"\x1b[35m"
# define AGSI_CYAN	"\x1b[36m"
# define AGSI_WHT	"\x1b[37m"
# define AGSI_BR_BLK	"\x1b[90m"
# define AGSI_BR_RED	"\x1b[91m"
# define AGSI_BR_GRN	"\x1b[92m"
# define AGSI_BR_YEL	"\x1b[93m"
# define AGSI_BR_BLU	"\x1b[94m"
# define AGSI_BR_MAG	"\x1b[95m"
# define AGSI_BR_CYAN	"\x1b[96m"
# define AGSI_BR_WHT	"\x1b[97m"
# define AGSI_BLK_BG	"\x1b[40m"
# define AGSI_RED_BG	"\x1b[41m"
# define AGSI_GRN_BG	"\x1b[42m"
# define AGSI_YEL_BG	"\x1b[43m"
# define AGSI_BLU_BG	"\x1b[44m"
# define AGSI_MAG_BG	"\x1b[45m"
# define AGSI_CYAN_BG	"\x1b[46m"
# define AGSI_WHT_BG	"\x1b[47m"
# define AGSI_BR_BLK_BG	"\x1b[100m"
# define AGSI_BR_RED_BG	"\x1b[101m"
# define AGSI_BR_GRN_BG	"\x1b[102m"
# define AGSI_BR_YEL_BG	"\x1b[103m"
# define AGSI_BR_BLU_BG	"\x1b[104m"
# define AGSI_BR_MAG_BG	"\x1b[105m"
# define AGSI_BR_CYAN_BG "\x1b[106m"
# define AGSI_BR_WHT_BG	"\x1b[107m"

#else /* !AG_ANSI_COLOR */

# define AGSI_RST 	""
# define AGSI_BOLD     	""
# define AGSI_FAINT    	""
# define AGSI_ITALIC   	""
# define AGSI_UNDERLINE	""
# define AGSI_REVERSE  	""
# define AGSI_BLK	""
# define AGSI_RED	""
# define AGSI_GRN	""
# define AGSI_YEL	""
# define AGSI_BLU	""
# define AGSI_MAG	""
# define AGSI_CYA	""
# define AGSI_WHT	""
# define AGSI_BR_BLK	""
# define AGSI_BR_RED	""
# define AGSI_BR_GRN	""
# define AGSI_BR_YEL	""
# define AGSI_BR_BLU	""
# define AGSI_BR_MAG	""
# define AGSI_BR_CYA	""
# define AGSI_BR_WHT	""
# define AGSI_BLK_BG	""
# define AGSI_RED_BG	""
# define AGSI_GRN_BG	""
# define AGSI_YEL_BG	""
# define AGSI_BLU_BG	""
# define AGSI_MAG_BG	""
# define AGSI_CYA_BG	""
# define AGSI_WHT_BG	""
# define AGSI_BR_BLK_BG	""
# define AGSI_BR_RED_BG	""
# define AGSI_BR_GRN_BG	""
# define AGSI_BR_YEL_BG	""
# define AGSI_BR_BLU_BG	""
# define AGSI_BR_MAG_BG	""
# define AGSI_BR_CYA_BG	""
# define AGSI_BR_WHT_BG	""

#endif /* AG_ANSI_COLOR */