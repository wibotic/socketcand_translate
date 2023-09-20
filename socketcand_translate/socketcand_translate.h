/*  Socketcand CAN frame protocol translater.
    Used to mimic socketcand for primery use with Python-cans socketcand interface
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKETCAND_TRANSLATE_CLOSED 0
#define SOCKETCAND_TRANSLATE_OPEN 1
#define SOCKETCAND_TRANSLATE_ERR_FLAG 0xF0000000U // Unique error flag to socketcand translate
#define SOCKETCAND_TRANSLATE_EXT_MASK 0x1FFFFFFFU 
#define SOCKETCAND_TRANSLATE_STD_MASK 0x000007FFU 


//basic CAN frame structure used to pack and transport CAN frame data
struct socketcand_translate_frame
{
    uint32_t id;
    uint8_t len;
    uint8_t data[8]; 
    uint8_t ext;
};

// This function should be used to convert CAN data from it's raw seperate form into
// Socketcand's format, < frame [id] [secs].[usecs] [data] >   
int socketcand_translate_frame_to_string(char * buf, int bufsize, uint32_t id, unsigned long secs, unsigned long usecs, uint8_t data[8], int len, uint8_t ext);

// This function should be used to translate Socketcand send frames < send [id] [data_length] [data] >
// into a seperated format contained in the socketcand_translate_frame struct
struct socketcand_translate_frame socketcand_translate_string_to_frame(char * buf, struct socketcand_translate_frame f);

// This function should be used to open the socketcand translator
int socketcand_translate_open_raw(int round, char * buf, int bufsize);

// This function should be used to check if the socketcand translator has
// been opened correctly 
int socketcand_translate_is_open();

// This function is used to change the state
int socketcand_translate_set_state(int state);

// This function is used to get the ID length from a < send > frame to determine
// whether or not it is an extended or standard frame
int id_len(char* buf);

// To be determined by user. Called by socketcand_translate_frame_to_string().
// Defines unique behavior of where the socketcand frame goes after being converted
int socketcand_translate_string_out(char * s);

// To be determined by user. Called by socketcand_translate_string_to_frame().
// Defines unique behavior of where the socketcand frame goes after being converted.
int socketcand_translate_struct_out(struct socketcand_translate_frame f);

// internal use will delete on release
int send_formater(char * buf, long bufsize, unsigned int id, unsigned char data[8], int len);

