// PS5 only: no-op OpenGL entry points (the OpenGL renderer of gl.c is never used there; Direct3D 9 goes through DXVK/Vulkan).
// Provided here so the payload needs no libGL/libgallium at load time.
#ifdef __PROSPERO__
long glBeginQuery(void) { return 0; }
long glBindBuffer(void) { return 0; }
long glBindFramebufferEXT(void) { return 0; }
long glClear(void) { return 0; }
long glClearColor(void) { return 0; }
long glColorMask(void) { return 0; }
long glDeleteSync(void) { return 0; }
long glDisable(void) { return 0; }
long glEnable(void) { return 0; }
long glEndQuery(void) { return 0; }
long glGenQueries(void) { return 0; }
long glGetBufferParameteriv(void) { return 0; }
long glGetIntegerv(void) { return 0; }
long glGetQueryObjectiv(void) { return 0; }
long glGetQueryObjectui64vEXT(void) { return 0; }
long glGetTexLevelParameteriv(void) { return 0; }
long glMapBuffer(void) { return 0; }
long glPixelStorei(void) { return 0; }
long glPopAttrib(void) { return 0; }
long glPushAttrib(void) { return 0; }
long glReadBuffer(void) { return 0; }
long glReadPixels(void) { return 0; }
long glScissor(void) { return 0; }
long glUnmapBuffer(void) { return 0; }
long glUnmapBufferARB(void) { return 0; }
#endif
