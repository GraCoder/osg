#include <iomanip>
#include <sstream>
#include <stdio.h>

#include <osg/BlendEquation>
#include <osg/GLDefines>
#include <osg/Geometry>
#include <osg/Depth>
#include <osg/BlendFunc>
#include <osg/Texture2D>

#include <osgDB/FileUtils>

#include <osgViewer/ImGuiHandler>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>

#include <osgViewer/imgui/imgui.h>

namespace osgViewer {

constexpr char vert[] = R"(
#version 330 core 

layout(location = 0) in vec4 attr_Vertex;
layout(location = 1) in vec4 attr_Color;
layout(location = 2) in vec2 attr_UV;

uniform mat4 prjMatrix;

out vec4 vp_Color;
out vec2 vp_UV;

void main()
{
    gl_Position = prjMatrix * attr_Vertex; 

    vp_Color = attr_Color;
    vp_UV.x = attr_UV.x;
    vp_UV.y = attr_UV.y;
}
)";

constexpr char frag[] = R"(
#version 330 core 

in vec4 vp_Color;
in vec2 vp_UV;

uniform sampler2D tex;

out vec4 fragColor;

void main()
{
    vec4 albedo = texture2D(tex, vp_UV);
    fragColor = vp_Color * albedo;
}
)";

class UIRect : public osg::Drawable {
  osg::ref_ptr<osg::ByteArray> _vertex;
  osg::ref_ptr<osg::DrawElementsUShort> _ele;
  std::vector<std::tuple<int, int, osg::Vec4i>> _pris;
public:
  UIRect()
  {
    _vertex = new osg::ByteArray;
    _vertex->setBufferObject(new osg::VertexBufferObject);
    _ele = new osg::DrawElementsUShort;
    _ele->setBufferObject(new osg::ElementBufferObject);
  }

  void setVertex(int sz, ImDrawVert *pvert)
  {
    int len = sizeof(ImDrawVert) * sz;
    _vertex->resize(len);
    memcpy((void *)(_vertex->getDataPointer()), pvert, len);
    _vertex->dirty();
  }

  void setIndex(int sz, ImDrawIdx *idx)
  {
    _ele->assign(idx, idx + sz);
    _ele->dirty();
  }

  void allocatePrimitves(int sz) { _pris.resize(sz); }

  void setPrimitive(int idx, int offset, int num, const osg::Vec4i &scissor)
  {
    _pris[idx] = std::make_tuple(offset, num, scissor);
  }

  void drawImplementation(osg::RenderInfo &renderInfo) const 
  {
    auto state = renderInfo.getState();

    osg::VertexArrayState *vas = state->getCurrentVertexArrayState();
    vas->setVertexBufferObjectSupported(true);

    vas->lazyDisablingOfVertexAttributes();

    auto contextId = renderInfo.getContextID();

    auto vbo = _vertex->getOrCreateGLBufferObject(contextId);
    if(vbo->isDirty()) vbo->compileBuffer();
    vas->bindVertexBufferObject(vbo);
    vas->setVertexAttribArray(*state, 0, 2, GL_FLOAT, sizeof(ImDrawVert), 0);
    vas->setVertexAttribArray(*state, 1, 4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, col), true);
    vas->setVertexAttribArray(*state, 2, 2, GL_FLOAT, sizeof(ImDrawVert), (GLvoid *)offsetof(ImDrawVert, uv));

    auto ebo = _ele->getOrCreateGLBufferObject(contextId);
    if (ebo->isDirty()) ebo->compileBuffer();
    vas->bindElementBufferObject(ebo);

    vas->applyDisablingOfVertexAttributes(*state);

    for(int i = 0; i < _pris.size(); i++) {
      int oft, count;
      osg::Vec4i scissor;
      std::tie(oft, count, scissor) = _pris[i];
      glScissor(scissor.x(), scissor.y(), scissor.z(), scissor.w());
      glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, (GLvoid *)(intptr_t)oft);
    }

    vas->unbindVertexBufferObject();
    vas->unbindElementBufferObject();
  }
};

static ImGuiKey ConvertFromOSGKey(int key)
{
#define KEY osgGA::GUIEventAdapter::KeySymbol

  switch (key) {
  case KEY::KEY_Tab:
    return ImGuiKey_Tab;
  case KEY::KEY_Left:
    return ImGuiKey_LeftArrow;
  case KEY::KEY_Right:
    return ImGuiKey_RightArrow;
  case KEY::KEY_Up:
    return ImGuiKey_UpArrow;
  case KEY::KEY_Down:
    return ImGuiKey_DownArrow;
  case KEY::KEY_Page_Up:
    return ImGuiKey_PageUp;
  case KEY::KEY_Page_Down:
    return ImGuiKey_PageDown;
  case KEY::KEY_Home:
    return ImGuiKey_Home;
  case KEY::KEY_End:
    return ImGuiKey_End;
  case KEY::KEY_Delete:
    return ImGuiKey_Delete;
  case KEY::KEY_BackSpace:
    return ImGuiKey_Backspace;
  case KEY::KEY_Return:
    return ImGuiKey_Enter;
  case KEY::KEY_Escape:
    return ImGuiKey_Escape;
  default: // Not found
    return ImGuiKey_None;
  }
}

class ImGuiBegUpdate : public osg::Operation {
  ImGuiHandler *_handler = nullptr;
public:
  ImGuiBegUpdate(ImGuiHandler *handler) : _handler(handler){};
  void operator()(osg::Object *) { ImGui::NewFrame(); }
};

class ImGuiEndUpdate : public osg::Operation {
  ImGuiHandler *_handler = nullptr;
public:
  ImGuiEndUpdate(ImGuiHandler *handler) : _handler(handler){};
  void operator()(osg::Object *)
  {
    ImGui::EndFrame();
    ImGui::Render();

    _handler->setRenderData();
  }
};

class ImGuiRenderCallback : public osg::Camera::DrawCallback {
public:
  ImGuiRenderCallback(ImGuiHandler *handler)
    : _handler(handler)
  {
  }

  void operator()(osg::RenderInfo &renderInfo) const override
  {
    auto state = renderInfo.getState();

    auto frm = state->getFrameStamp()->getFrameNumber();
    auto geode = _handler->getUiGroup(frm);

    state->apply(geode->getStateSet());
    for(uint32_t i = 0; i < geode->getNumDrawables(); i++) {
      auto d = geode->getDrawable(i); 
      if (!d->getNodeMask())
        continue;
      d->draw(renderInfo);
    }
  }

private:
  ImGuiHandler *_handler = nullptr;
};

ImGuiHandler::ImGuiHandler()
  : _initialized(false)
  , _imctx(0)
{
  OSG_INFO << "ImGuiHandler::ImGuiHandler()" << std::endl;

  _begOp = new ImGuiBegUpdate(this);
  _endOp = new ImGuiEndUpdate(this);

  auto program = new osg::Program;
  program->addShader(new osg::Shader(osg::Shader::VERTEX, vert));
  program->addShader(new osg::Shader(osg::Shader::FRAGMENT, frag));

  program->addBindAttribLocation("attr_Vertex", 0);
  program->addBindAttribLocation("attr_Color", 1);
  program->addBindAttribLocation("attr_UV", 2);

  for (int i = 0; i < 2; i++) {
    auto geode = new osg::Geode;
    auto ss = geode->getOrCreateStateSet();
    ss->setRenderBinDetails(INT_MAX, "RenderBin");

    {
      auto depth = new osg::Depth;
      depth->setWriteMask(0);
      ss->setAttributeAndModes(depth, osg::StateAttribute::OFF);
    }

    {
      auto blend = new osg::BlendFunc; 
      ss->setAttributeAndModes(blend, osg::StateAttribute::ON);
    }

    ss->setAttribute(program);
    _geodes[i] = geode;
  }

  initImGui();
}

ImGuiHandler::~ImGuiHandler()
{
  if (_imctx) {
    ImGui::DestroyContext(_imctx);
    _imctx = nullptr;
  }
}

bool ImGuiHandler::handle(const osgGA::GUIEventAdapter &ea, osgGA::GUIActionAdapter &aa)
{
  auto viewer = static_cast<osgViewer::Viewer *>(&aa);
  auto vp = viewer->getCamera()->getViewport();

  ImGuiIO &io = ImGui::GetIO();
  if (!_initialized) {
    _initialized = true;
    _width = vp->width(); _height = vp->height(); resizeViewport();
    viewer->getCamera()->addPostDrawCallback(new ImGuiRenderCallback(this));
  }

  io.DisplaySize = ImVec2(vp->width(), vp->height());

  const bool wantCaptureMouse = io.WantCaptureMouse;
  const bool wantCaptureKeyboard = io.WantCaptureKeyboard;

  switch (ea.getEventType()) {
  case osgGA::GUIEventAdapter::KEYDOWN:
  case osgGA::GUIEventAdapter::KEYUP: {
    const bool isKeyDown = ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN;
    const int c = ea.getKey();

    // Always update the mod key status.
    io.KeyCtrl = ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_CTRL;
    io.KeyShift = ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SHIFT;
    io.KeyAlt = ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_ALT;
    io.KeySuper = ea.getModKeyMask() & osgGA::GUIEventAdapter::MODKEY_SUPER;

    auto imgui_key = ConvertFromOSGKey(c);
    io.AddKeyEvent(imgui_key, isKeyDown);

    return wantCaptureKeyboard;
  }
  case (osgGA::GUIEventAdapter::CHAR): {
    unsigned int k = ea.getKey();
    io.AddInputCharacterUTF16(k);
  }
  case (osgGA::GUIEventAdapter::RELEASE): {
    io.MousePos = ImVec2(ea.getX(), io.DisplaySize.y - ea.getY());
    if (ea.getButton() & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
      io.AddMouseButtonEvent(0, false);
    if (ea.getButton() & osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON)
      io.AddMouseButtonEvent(1, false);
    if (ea.getButton() & osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON)
      io.AddMouseButtonEvent(2, false);
    return wantCaptureMouse;
  }
  case (osgGA::GUIEventAdapter::PUSH): {
    io.MousePos = ImVec2(ea.getX(), io.DisplaySize.y - ea.getY());
    if (ea.getButtonMask() & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
      io.AddMouseButtonEvent(0, true);
    else if (ea.getButtonMask() & osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON)
      io.AddMouseButtonEvent(1, true);
    else if (ea.getButtonMask() & osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON)
      io.AddMouseButtonEvent(2, true);
    return wantCaptureMouse;
  }
  case (osgGA::GUIEventAdapter::DOUBLECLICK): {
    io.MousePos = ImVec2(ea.getX(), io.DisplaySize.y - ea.getY());
    io.MouseDoubleClicked[0] = ea.getButtonMask() & osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON;
    io.MouseDoubleClicked[1] = ea.getButtonMask() & osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON;
    io.MouseDoubleClicked[2] = ea.getButtonMask() & osgGA::GUIEventAdapter::MIDDLE_MOUSE_BUTTON;
    return wantCaptureMouse;
  }
  case (osgGA::GUIEventAdapter::DRAG):
  case (osgGA::GUIEventAdapter::MOVE): {
    io.AddMousePosEvent(ea.getX(), io.DisplaySize.y - ea.getY());
    return wantCaptureMouse;
  }
  case (osgGA::GUIEventAdapter::SCROLL): {
    io.AddMouseWheelEvent(0, ea.getScrollingMotion() == osgGA::GUIEventAdapter::SCROLL_UP ? 1.0 : -1.0);
    return wantCaptureMouse;
  }
  case (osgGA::GUIEventAdapter::FRAME): {
    viewer->addPreUpdateOperation(_begOp);
    viewer->addPstUpdateOperation(_endOp);
    _frameNum = viewer->getFrameStamp()->getFrameNumber();
    ImGui::SetCurrentContext(_imctx);
    break;
  }
  case (osgGA::GUIEventAdapter::RESIZE): {
    _width = ea.getWindowWidth();
    _height = ea.getWindowHeight();
    resizeViewport();
    break;
  }
  case (osgGA::GUIEventAdapter::CLOSE_WINDOW): {
  }
  default:
    break;
  }
  return false;
}

void ImGuiHandler::setRenderData() 
{
  auto io = ImGui::GetIO();
  auto draw = ImGui::GetDrawData();

  int fbwidth = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x);
  int fbheight = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
  if (fbwidth == 0 || fbheight == 0)
    return;
  draw->ScaleClipRects(io.DisplayFramebufferScale);

  auto grp = getUiGroup(_frameNum);
  if(grp->getNumDrawables() < uint32_t(draw->CmdListsCount)) {
    int num = draw->CmdListsCount - grp->getNumDrawables(); 
    for (int i = 0; i < num; i++)
      grp->addChild(new UIRect);
  } 

  for (uint32_t i = 0; i < grp->getNumDrawables(); i++)
    grp->getDrawable(i)->setNodeMask(0);

  for (int i = 0; i < draw->CmdListsCount; i++) {
    auto cmds = draw->CmdLists[i];
    auto rect = static_cast<UIRect *>(grp->getDrawable(i));
    rect->setNodeMask(~0u);
    rect->setVertex(cmds->VtxBuffer.Size, cmds->VtxBuffer.Data);
    rect->setIndex(cmds->IdxBuffer.Size, cmds->IdxBuffer.Data);

    rect->allocatePrimitves(cmds->CmdBuffer.size());
    for(int j = 0; j < cmds->CmdBuffer.size(); j++) {
      auto &cmd = cmds->CmdBuffer[j];
      osg::Vec4i scissor(cmd.ClipRect.x, cmd.ClipRect.y, 
        (uint16_t)(cmd.ClipRect.z - cmd.ClipRect.x), (uint16_t)(cmd.ClipRect.w - cmd.ClipRect.y));
      rect->setPrimitive(j, cmd.IdxOffset * sizeof(ImDrawIdx), cmd.ElemCount, scissor);
    }
  }
}

osg::Geode *ImGuiHandler::getUiGroup(uint32_t frm)
{
  int index = frm % 2;
  return _geodes[index];
}

void ImGuiHandler::initImGui()
{
  //--------------------create imgui context----------------------------------
  _imctx = ImGui::CreateContext();

  ImGui::StyleColorsDark();
  ImGuiIO &io = ImGui::GetIO();

  std::vector<std::string> fontFiles;
  fontFiles.push_back("C:\\Users\\t\\AppData\\Local\\Microsoft\\Windows\\Fonts\\SourceHanSansSC-Regular.otf");
  fontFiles.push_back("C:\\Windows\\Fonts\\simhei.ttf");

  for (auto &fontPath : fontFiles) {
    if (osgDB::fileExists(fontPath)) {
      ImFont *font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, NULL, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
      IM_UNUSED(font);
      break;
    }
  }

  int width, height;
  unsigned char *pixels;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  auto img = new osg::Image;
  img->setImage(width, height, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, pixels, osg::Image::NO_DELETE);
  auto tex = new osg::Texture2D;
  tex->setImage(img);
  tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
  tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
  for(int i = 0; i < 2 ;i++) {
    auto ss = _geodes[i]->getStateSet(); 
    ss->setTextureAttributeAndModes(0, tex);
    ss->getOrCreateUniform("tex", osg::Uniform::SAMPLER_2D)->set(0);
  }

  //{
  //	io.KeyMap[ImGuiKey_Tab] = ImGuiKey_Tab;
  //	io.KeyMap[ImGuiKey_LeftArrow] = ImGuiKey_LeftArrow;
  //	io.KeyMap[ImGuiKey_RightArrow] = ImGuiKey_RightArrow;
  //	io.KeyMap[ImGuiKey_UpArrow] = ImGuiKey_UpArrow;
  //	io.KeyMap[ImGuiKey_DownArrow] = ImGuiKey_DownArrow;
  //	io.KeyMap[ImGuiKey_PageUp] = ImGuiKey_PageUp;
  //	io.KeyMap[ImGuiKey_PageDown] = ImGuiKey_PageDown;
  //	io.KeyMap[ImGuiKey_Home] = ImGuiKey_Home;
  //	io.KeyMap[ImGuiKey_End] = ImGuiKey_End;
  //	io.KeyMap[ImGuiKey_Delete] = ImGuiKey_Delete;
  //	io.KeyMap[ImGuiKey_Backspace] = ImGuiKey_Backspace;
  //	io.KeyMap[ImGuiKey_Enter] = ImGuiKey_Enter;
  //	io.KeyMap[ImGuiKey_Escape] = ImGuiKey_Escape;
  //	io.KeyMap[ImGuiKey_A] = osgGA::GUIEventAdapter::KeySymbol::KEY_A;
  //	io.KeyMap[ImGuiKey_C] = osgGA::GUIEventAdapter::KeySymbol::KEY_C;
  //	io.KeyMap[ImGuiKey_V] = osgGA::GUIEventAdapter::KeySymbol::KEY_V;
  //	io.KeyMap[ImGuiKey_X] = osgGA::GUIEventAdapter::KeySymbol::KEY_X;
  //	io.KeyMap[ImGuiKey_Y] = osgGA::GUIEventAdapter::KeySymbol::KEY_Y;
  //	io.KeyMap[ImGuiKey_Z] = osgGA::GUIEventAdapter::KeySymbol::KEY_Z;

  //	//io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  //}
}

void ImGuiHandler::resizeViewport() 
{
  for (int i = 0; i < 2; i++) 
  {
    auto ss = _geodes[i]->getOrCreateStateSet();
    auto m = osg::Matrixf::ortho2D(0, _width, _height, 0);
    ss->getOrCreateUniform("prjMatrix", osg::Uniform::FLOAT_MAT4)->set(m);
  }
}

} // namespace osgViewer
