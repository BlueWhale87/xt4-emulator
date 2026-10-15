#include "xt4_face.h"
int main(void){
 xt4_face_context*ctx=(xt4_face_context*)(uintptr_t)1;
 return xt4_face_create(0,0,0,&ctx)==XT4_FACE_INVALID&&ctx==0?0:1;
}
