TXT(#version 110\n#extension GL_ARB_draw_instanced : enable\n
uniform mat4 MVP[100];\n
attribute vec2 vPos;\n

void main()\n
{\n
  gl_Position = MVP[gl_InstanceID] * vec4(vPos.xy, 0, 1);\n
}\n
)