TXT(#version 110\n#extension GL_ARB_draw_instanced : enable\n
uniform mat4 MVP[100];\n
uniform vec2 Inflate[100];\n
attribute vec2 vPos;\n
varying vec2 pos;

void main()\n
{\n
  pos = vPos.xy;\n
  pos -= vec2(0.5,0.5);\n
  pos *= Inflate[gl_InstanceID];\n
  pos += vec2(0.5,0.5);\n
  gl_Position = MVP[gl_InstanceID] * vec4(pos, 0, 1);\n
}\n
)