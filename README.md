# About
The server is initiated by `nc -C hostname PORT` and then new players can join the server by connecting to the same port. Once any player has joined, the server chooses a random word out of the dictionary and prompts the user to guess it. New players join in whenever; they are put in a queue for their subsequent turn. In one game-over state, the winner is announced when the last player guesses the last correct letter. Otherwise, a draw is announced when the number of guesses are exhausted and the word has not been fully guessed yet. Players can disconnect at any time and the game is resumed as usual.

This was the final assignment for the course, CSC209.
