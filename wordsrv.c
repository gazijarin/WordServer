#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 52061
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void announce_turn(struct game_state *game);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);
/* Handle inputted name from a new player */
void handle_client_name(struct client *p, struct client **new_player_list,
                        struct game_state *game);
/* Handle inputted guess from an active player */
void handle_client_guess(struct client *p, struct game_state *game);
/* Check if name is already in player list */
int check_name(char *name, struct game_state *game);
/* Count number of players in player list */
int count_players(struct game_state *game);
/* Search and return player */
struct client *search(int fd, struct game_state *game);
/* Move player from the new player list to the game. */
void move_player(struct client **new_player_list, struct client *player,
                  struct game_state *game, int fd);
/* Start a new game. */
void new_game(struct game_state *game);
/* Display the current gameboard. */
void display_game(struct game_state *game, int fd);
/* Disconnect player from new player list. */
void disconnect_newplayer(struct client **new_player_list, struct client *p);
/* Disconnect player from active list. */
void disconnect_activeplayer(struct game_state *game, struct client *p);
/* Find network newline in buf. */
int find_network_newline(const char *buf, int n);

/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

/* Display the current gameboard to client with fd. */
void display_game(struct game_state *game, int fd) {
  char game_display[MAX_MSG];
  char *game_display_message = status_message(game_display, game);

  struct client *p;
  for (p = game->head; p != NULL; p = p->next) {
    if (p->fd == fd) {
      if (write(p->fd, game_display_message,
                strlen(game_display_message)) == -1) {
        fprintf(stderr, "Write to client failed\n");
        disconnect_activeplayer(game, p);
      };
    }
  }
}

/* Disconnect a new player from new_player_list. */
void disconnect_newplayer(struct client **new_player_list, struct client *p) {
  int dp_fd = p->fd;

  struct client **curr_p;
  for (curr_p = new_player_list; *curr_p && (*curr_p)->fd != dp_fd;
       curr_p = &(*curr_p)->next)
      ;
  if (*curr_p) {
    struct client *t = (*curr_p)->next;
    printf("Removing client %d %s\n", dp_fd, inet_ntoa((*curr_p)->ipaddr));
    FD_CLR(p->fd, &allset);
    close((*curr_p)->fd);
    free(*curr_p);
    *curr_p = t;
  }
  else {
    fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
    dp_fd);
  }
}

/* Disconnect an active player from the game. */
void disconnect_activeplayer(struct game_state *game, struct client *p) {
  char goodbye_msg[MAX_MSG];
  sprintf(goodbye_msg, "%s left the game.\r\n", p->name);

  int dp_fd = p->fd;

  // If the next turn is the player who disconnected, advance the turn.
  if (game->has_next_turn != NULL && game->has_next_turn->fd == dp_fd) {
      advance_turn(game);
  }
  // If the game has only one player, reset the game head and the next turn.
  if (count_players(game) == 1) {
    game->head = NULL;
    game->has_next_turn = NULL;
    printf("Removing client %d %s\n", dp_fd, inet_ntoa(p->ipaddr));
    FD_CLR(p->fd, &allset);
    close(p->fd);
    free(p);
  }
  // If the player is in the front of the linked list, replace the front.
  else if (game->head != NULL && game->head->fd == dp_fd) {
    struct client *front = p->next;
    game->head = front;
    printf("Removing client %d %s\n", dp_fd, inet_ntoa(p->ipaddr));
    FD_CLR(p->fd, &allset);
    close(p->fd);
    free(p);
  }
  // If the player is the in the tail of the linked list, replace the tail.
  else if (p->next == NULL) {
    struct client *back;
    for (back = game->head; back->next->fd!= dp_fd; back = back->next);
    back->next = NULL;
    printf("Removing client %d %s\n", dp_fd, inet_ntoa(p->ipaddr));
    FD_CLR(p->fd, &allset);
    close(p->fd);
    free(p);
  }
  // If the player is in the middle, replace the players before and after.
  else {
    struct client *next = p->next;
    struct client *back;
    for (back = game->head; back->next->fd != dp_fd; back = back->next);
    back->next = next;
    printf("Removing client %d %s\n", dp_fd, inet_ntoa(p->ipaddr));
    FD_CLR(p->fd, &allset);
    close(p->fd);
    free(p);
    }
    // Announce that the player has left if there are still players in the game.
    if (count_players(game) > 0) {
      broadcast(game, goodbye_msg);
      announce_turn(game);
    }
}

/*
 * Create a new game after the previous game finished.
 */
void new_game(struct game_state *game) {
  char buf[MAX_WORD];
  rewind(game->dict.fp);

  // Choose a random word.
  int index = random() % game->dict.size;
  printf("Looking for word at index %d\n", index);
  for(int i = 0; i <= index; i++) {
      if(!fgets(buf, MAX_WORD, game->dict.fp)){
          fprintf(stderr,"File ended before we found the entry index %d",index);
          exit(1);
      }
  }

  if(buf[strlen(buf) - 1] == '\n') {
      buf[strlen(buf) - 1] = '\0';
  } else {
      fprintf(stderr,
        "The dictionary file does not appear to have Unix line endings\n");
  }
  strncpy(game->word, buf, MAX_WORD);
  game->word[MAX_WORD-1] = '\0';
  for(int j = 0; j < strlen(game->word); j++) {
      game->guess[j] = '-';
  }
  game->guess[strlen(game->word)] = '\0';

  for(int i = 0; i < NUM_LETTERS; i++) {
      game->letters_guessed[i] = 0;
  }
  game->guesses_left = MAX_GUESSES;
}

/*
 * Advance the turn and adjust the one who has the next turn.
 */
void advance_turn(struct game_state *game) {
  if (count_players(game) > 1) {
    // If the player is the head of the list, then advance the turn to the
    // player in the tail of the list.
    if ((game->has_next_turn->fd) == (game->head->fd)) {
      struct client *c;
      for (c = game->head; c->next != NULL; c = c->next);
      game->has_next_turn = c;
    }
    // If the player is in the middle of the list, then advance the turn to
    // the player before them.
    else {
      struct client *p;
      for (p = game->head; ((p != NULL)
                            && (p->next->fd != game->has_next_turn->fd));
                            p = p->next);
      game->has_next_turn = p;
    }
  }
  if (count_players(game) >= 1) {
    announce_turn(game);
  }
}

/*
 * Announce whose turn it is, based on who has the next turn.
 */
void announce_turn(struct game_state *game) {
  // String to declare whose turn it is.
  char turn[MAX_MSG];
  sprintf(turn, "It's %s's turn.\r\n", (game->has_next_turn)->name);
  char *guess_msg = "Your guess?\r\n";
  // Strings to declare the outcome of the game.
  char win[MAX_MSG];
  sprintf(win, "Game over. %s won!\r\n", (game->has_next_turn)->name);
  char *game_over = "Game over. You've exhausted all the guesses.\r\n";
  // String to reveal the word once the game is over.
  char word[MAX_MSG];
  sprintf(word, "The word was %s.\r\n", game->word);
  // String to announce a new game.
  char *new_game_msg = "Let's start a new game.\r\n";
  // String to display the current gameboard.
  char game_display[MAX_MSG];

  // Announce turn iff there are still guesses left and the word has not
  // been guessed yet.
  if ((game->guesses_left > 0) && (strcmp(game->word, game->guess) != 0)) {
    struct client *p;
    for (p = game->head; p != NULL; p = p->next) {
      // If the player does not have the next turn, announce whose turn it is.
      if (p->fd != (game->has_next_turn)->fd) {
        if (write(p->fd, turn, strlen(turn)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_activeplayer(game, p);
        };
      }
      // If the player does have the next turn, prompt guess.
      else {
        if(write(p->fd, guess_msg, strlen(guess_msg)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_activeplayer(game, p);
        };
      }
    }
  }
  // Game over
  else {
    if (strcmp(game->word, game->guess) == 0) {
      broadcast(game, word);
      broadcast(game, win);
    }
    else {
      broadcast(game, word);
      broadcast(game, game_over);
    }
    // Create a new game.
    broadcast(game, new_game_msg);
    new_game(game);
    char *game_display_message = status_message(game_display, game);
    broadcast(game, game_display_message);
    announce_turn(game);
  }
}

/*
 * Move a player from the new_player_list to the active game list.
 */
void move_player(struct client **new_player_list, struct client *player,
                 struct game_state *game, int fd) {
  // Add the player to the game head.
  struct in_addr addr;
  addr.s_addr = player->ipaddr.s_addr;
  add_player(&(game->head), fd, addr);
  struct client *game_player = search(fd, game);
  strcpy(game_player->name, player->name);

  // Remove the player from the new player list.
  struct client **curr_p;
  for (curr_p = new_player_list; *curr_p && (*curr_p)->fd != fd;
       curr_p = &(*curr_p)->next)
      ;
  if (*curr_p) {
    struct client *t = (*curr_p)->next;
    printf("Removing client %d %s\n", fd, inet_ntoa((*curr_p)->ipaddr));
    *curr_p = t;
  }
  else {
    fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
             fd);
  }
}

/*
 * Move a player from the new_player_list to the active game list.
 */
struct client *search(int fd, struct game_state *game) {
  struct client *p;
  for (p = game->head; p != NULL; p = p->next) {
    if (p->fd == fd) {
      return p;
    }
  }
  return p;
}

/*
 * Count the number of active players in the current game.
 */
int count_players(struct game_state *game) {
  struct client *p;
  int num_players = 0;
  for (p = game->head; p != NULL; p = p->next) {
      num_players += 1;
  }
  return num_players;
}

/*
 * Broadcast outbuf to everyone in the game.
 */
void broadcast(struct game_state *game, char *outbuf) {
    struct client *p;
    for (p = game->head; p != NULL; p = p->next) {
        if (write(p->fd, outbuf, strlen(outbuf)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_activeplayer(game, p);
        };
    }
}

/*
 * Check if there is a player with the same name in the game.
 */
int check_name(char *name, struct game_state *game) {
  struct client *p;
  for (p = game->head; p != NULL; p = p->next) {
    if (strcmp(p->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/*
 * Search the first n characters of buf for a network newline (\r\n).
 * Return one plus the index of the '\n' of the first network newline,
 * or -1 if no network newline is found.
 */
int find_network_newline(const char *buf, int n) {
    int i = 0;
    while (i < n - 1) {
        if (buf[i] == '\r' && buf[i+1] == '\n') {
            return i + 2;
        }
        i++;
    }
    return -1;
}

/*
 * Handle the client's guess.
 */
void handle_client_guess(struct client *p, struct game_state *game) {
  // Strings to inform the player about their guesses.
  char *empty_guess_msg = "Please, enter a non-empty guess.\r\n";
  char *single_guess_msg = "Please, enter a single guess.\r\n";
  char *already_in_word = "The letter has already been guessed. Try again.\r\n";
  char *lower_case_msg = "The letter should be in lower-case.\r\n";
  char *incorrect_guess = "That was an incorrect guess.\r\n";
  char *not_turn = "It's not your turn to guess.\r\n";

  // A string to hold the whole guess.
  char *whole_line = malloc(sizeof(char) * (MAX_BUF + 1));
  if (whole_line == NULL) {
      perror("malloc");
      exit(1);
  }
  whole_line[0] = '\0';
  int len_guess, len, where, inbuf, room;
  int newline_flag = 1;
  int disconnect_flag = 1;
  int red_flag = 1;
  // Call select and read until a newline character is found.
  while (newline_flag == 1) {
    inbuf = 0;
    room = sizeof(p->inbuf);

    len = read(p->fd, p->in_ptr, room);

    if (len == 0) {
        disconnect_flag = 0;
        disconnect_activeplayer(game, p);
        break;
    }
    else if (len < 0) {
      fprintf(stderr, "Read from client failed\n");
      remove_player(&(game->head), p->fd);
    }

    inbuf += len;
    if ((where = find_network_newline(p->inbuf, inbuf)) < 0) {
        newline_flag = 1;
        room -= len;
    }
    else {
      newline_flag = 0;
    }
    strncat(whole_line, p->inbuf, room);
    room -= len;
  }
  // Add null terminator to the whole_line inputted by user.
  if (newline_flag == 0) {
    for (int i = 0; i < MAX_BUF; i++) {
      if (!((whole_line[i] >= 'a' && whole_line[i] <= 'z')
          || (whole_line[i] >= 'A' && whole_line[i] <= 'Z'))) {
            whole_line[i] = '\0';
            break;
          }
    }
    len_guess = strlen(whole_line);
  }
  else {
    whole_line[0] = '\0';
    len_guess = strlen(whole_line);
  }

  // If the player has not disconnected and it's not their turn, inform.
  if (disconnect_flag == 1 && (game->has_next_turn->fd != p->fd)) {
      if(write(p->fd, not_turn, strlen(not_turn)) == -1) {
        fprintf(stderr, "Write to client failed\n");
        disconnect_activeplayer(game, p);
      };
  }
  // If the player has not disconnected and it's their turn, process the guess.
  else if (disconnect_flag == 1 && (game->has_next_turn->fd == p->fd)) {
    // Empty guess.
    if (len_guess == 0) {
      p->inbuf[0] = '\0';
      whole_line[0] = '\0';
      if(write(p->fd, empty_guess_msg, strlen(empty_guess_msg)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_activeplayer(game, p);
      };
    }
    // Guess with more than one character.
    else if ((len_guess) > 1) {
      p->inbuf[0] = '\0';
      whole_line[0] = '\0';
      if(write(p->fd, single_guess_msg, strlen(single_guess_msg)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_activeplayer(game, p);
      };
    }
    // A single guess.
    else if ((len_guess) == 1) {
      int letter;
      letter = *(whole_line);
      // Check if the letter is lower-case.
      if (islower(letter) == 0) {
        p->inbuf[0] = '\0';
        whole_line[0] = '\0';
        inbuf -= where;
        if(write(p->fd, lower_case_msg, strlen(lower_case_msg)) == -1) {
            fprintf(stderr, "Write to client failed\n");
            disconnect_activeplayer(game, p);
        };
      }
      else {
        char guess = *whole_line;
        int position = guess - 'a';
        // Check if the letter has already been guessed.
        if (game->letters_guessed[position] == 1) {
          inbuf -= where;
          p->inbuf[0] = '\0';
          whole_line[0] = '\0';
          if(write(p->fd, already_in_word, strlen(already_in_word)) == -1) {
              fprintf(stderr, "Write to client failed\n");
              disconnect_activeplayer(game, p);
          };
        }
        // A valid guess. Proper format and has not already been guessed.
        else {
          red_flag = 0;
          printf("[%d] Read %d bytes\n", p->fd, inbuf);
          inbuf -= where;
          printf("[%d] Found newline %s\n", p->fd, p->inbuf);
        }
      }
    }
  }
  // String to hold the current guess.
  char announce_guess[MAX_BUF];

  if (red_flag == 0 && (game->has_next_turn->fd == p->fd)) {
    char guess = *(whole_line);
    int position = guess - 'a';
    int in_word = 0;

    for (int j = 0; j < MAX_WORD; j++) {
      if (game->word[j] == guess) {
        in_word = 1;
        game->guess[j] = guess;
      }
    }
    // Correct guess.
    if (in_word) {
      game->guesses_left -= 1;
      game->letters_guessed[position] = 1;
      printf("That was a correct guess by %s.\n", p->name);
      sprintf(announce_guess, "%s guesses: %c\r\n", p->name, guess);
      broadcast(game, announce_guess);
      display_game(game, p->fd);
      announce_turn(game);
    }
    // Incorrect guess.
    else {
      game->guesses_left -= 1;
      game->letters_guessed[position] = 1;
      if(write(p->fd, incorrect_guess, strlen(incorrect_guess)) == -1) {
         fprintf(stderr, "Write to client failed\n");
         disconnect_activeplayer(game, p);
       };
       printf("That was an incorrect guess by %s.\n", p->name);
       sprintf(announce_guess, "%s guesses: %c\r\n", p->name, guess);
       broadcast(game, announce_guess);
       display_game(game, p->fd);
       if (game->guesses_left == 0 || strcmp(game->word, game->guess) == 0) {
         announce_turn(game);
       }
       else {
         advance_turn(game);
       }
    }
  }
  free(whole_line);
}

/*
 * Handle the client's name.
 */
void handle_client_name(struct client *p, struct client **new_player_list,
                        struct game_state *game) {
  // String to hold the whole name.
  char *whole_name = malloc(sizeof(char) * MAX_BUF + 1);
  if (whole_name == NULL) {
      perror("malloc");
      exit(1);
  }
  whole_name[0] = '\0';
  // String to let the player know it was an invalid name.
  char *valid_name_msg = "Please, enter a valid name.\r\n";
  // String to let the players a new player has joined.
  char join[MAX_BUF];

  int inbuf, room, len_guess, len, where;
  int newline_flag = 1;
  int red_flag = 1;
  int disconnect_flag = 1;

  // Select and read till we encounter a newline.
  while (newline_flag == 1) {
    inbuf = 0;
    room = sizeof(p->inbuf);

    len = read(p->fd, p->in_ptr, room);
    // Disconnect if the player pressed CTRL-C.
    if (len == 0) {
        disconnect_flag = 0;
        disconnect_newplayer(new_player_list, p);
        break;
    }
    else if (len < 0) {
      fprintf(stderr, "Read from client failed\n");
      remove_player(new_player_list, p->fd);
    }
    inbuf += len;

    if ((where = find_network_newline(p->inbuf, inbuf)) < 0) {
        newline_flag = 1;
        room -= len;
    }
    else {
      newline_flag = 0;
    }
    strncat(whole_name, p->inbuf, room);
    room -= len;
  }
  // Add a null terminator to the whole name inputted by user.
  if (newline_flag == 0) {
    for (int i = 0; i < MAX_BUF; i++) {
      if (!((whole_name[i] >= 'a' && whole_name[i] <= 'z')
          || (whole_name[i] >= 'A' && whole_name[i] <= 'Z'))) {
            whole_name[i] = '\0';
            break;
          }
    }
    len_guess = strlen(whole_name);
  }
  else {
    whole_name[0] = '\0';
    len_guess = strlen(whole_name);
  }

  // Process the name if the player has not disconnected.
  if (disconnect_flag == 1) {
    // Empty name.
    if (len_guess == 0) {
      p->inbuf[0] = '\0';
      whole_name[0] = '\0';
      if(write(p->fd, valid_name_msg, strlen(valid_name_msg)) == -1) {
          fprintf(stderr, "Write to client failed\n");
          disconnect_newplayer(new_player_list, p);
      };
    }
    // Proper name format.
    else if (len_guess > 0) {
      // Check if any other player shares the same name.
      if (check_name(whole_name, game) == 1) {
        p->inbuf[0] = '\0';
        whole_name[0] = '\0';
        inbuf -= where;
        if(write(p->fd, valid_name_msg, strlen(valid_name_msg)) == -1) {
            fprintf(stderr, "Write to client failed\n");
            disconnect_newplayer(new_player_list, p);
        };
      }
      // Valid name.
      else {
        red_flag = 0;
        printf("[%d] Read %d bytes\n", p->fd, inbuf);
        inbuf -= where;
        printf("[%d] Found newline %s\n", p->fd, whole_name);
        p->inbuf[0] = '\0';
      }
    }
  }
  // If it is a valid name, strcpy it to the player's name and then
  // move the player to the active game list, announcing the player's
  // arrival and displaying the gameboard to them.
  if (red_flag == 0) {
     strcpy(p->name, whole_name);
     move_player(new_player_list, p, game, p->fd);
     sprintf(join, "%s has just joined.\r\n", p->name);
     broadcast(game, join);
     if (count_players(game) == 1) {
       game->has_next_turn = p;
     }
     display_game(game, p->fd);
     announce_turn(game);
  }
  free(whole_name);
}

/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}


int main(int argc, char **argv) {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
      perror("sigaction");
      exit(1);
    }

    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;

    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }

    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);

    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;

    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;

    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);

    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd);
            };
        }

        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        handle_client_guess(p, &game);
                        break;
                    }
                }

                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        // Handle client's name and whether it's acceptable.
                        handle_client_name(p, &new_players, &game);
                        break;
                    }
                }
            }
        }
    }
    return 0;
}
