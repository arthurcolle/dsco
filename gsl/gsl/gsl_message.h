/* gsl_message.h — Message Handling */
#ifndef __GSL_MESSAGE_H__
#define __GSL_MESSAGE_H__

#define GSL_MESSAGE_MASK_A  1
#define GSL_MESSAGE_MASK_B  2
#define GSL_MESSAGE_MASK_C  4
#define GSL_MESSAGE_MASK_D  8
#define GSL_MESSAGE_MASK_E  16
#define GSL_MESSAGE_MASK_F  32
#define GSL_MESSAGE_MASK_G  64
#define GSL_MESSAGE_MASK_H  128

void gsl_message(const char *message, const char *file, int line, unsigned int mask);
int gsl_message_mask(unsigned int mask);

#endif /* __GSL_MESSAGE_H__ */