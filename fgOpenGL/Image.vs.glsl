TXT(#version 110\n#extension GL_ARB_draw_instanced : enable\n
uniform mat4 MVP[100];\n
attribute vec4 vPosUV;\n
attribute vec4 vColor;\n
varying vec2 uv;\n
varying vec4 color;\n

void main()\n
{\n
  gl_Position = MVP[gl_InstanceID] * vec4(vPosUV.xy, 0, 1);\n
  uv = vPosUV.zw;\n
  color = vColor;\n
}\n
)