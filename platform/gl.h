#pragma once

// OpenGL 4.6 core with prototypes: ps5-opengl's static SDK exports every core command, and on the
// desktop libGL does too, so neither build needs a function loader.
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/gl.h>
#include <GL/glext.h>
