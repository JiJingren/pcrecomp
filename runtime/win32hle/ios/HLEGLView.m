/*
 * HLEGLView.m - the EAGL layer, and the touch handling.
 *
 * The drawing is deliberately thin. screen_ios.c has already turned the game's
 * primary surface into an RGBA8888 picture and published it; all that is left is
 * to put it in a texture and stretch it into the view, the way screen.c's SDL
 * renderer did, with the same five scaling modes and the same bars beside a
 * letterboxed picture. Two quads and one shader.
 *
 * Input is the interesting half, because a 1999 DirectDraw game has to be driven
 * by something it recognises: a mouse and a keyboard. A finger becomes the left
 * button - a press, moves, a release - and a tap is a press and a release in the
 * same place, twice in a row when it lands twice quickly. A second finger, or a
 * long press, is the right button, which is what the game's context menus want.
 * Three fingers cycle the scaling mode, since there is no F12.
 *
 * There is no vertex array object: the iOS 6 SDK's ES2 headers declare only the
 * OES spelling of those functions, and the buffer stays bound for the life of the
 * program, so the attribute pointers are set once and left.
 */
#import "HLEGLView.h"

#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CAEAGLLayer.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>

#include "screen_ios.h"
#include "hle_hid.h"

/* ---- the drawing ---- */

static const char *kVert =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec4 u_rect;\n"                 /* x, y, w, h in view pixels */
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4((a_pos.x - u_rect.x) / u_rect.z * 2.0 - 1.0,\n"
    "                    1.0 - (a_pos.y - u_rect.y) / u_rect.w * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

/* The crt mode is screen.c's: the lower third of every game row a shade darker,
 * so a 1999 picture looks like it is on a tube. Doing it here instead of with
 * 600 filled rectangles is the only reason this needs no second pass. */
static const char *kFrag =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_crt;\n"
    "uniform float u_h;\n"
    "uniform vec3 u_tint;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
    "  if (u_crt > 0.5 && fract(v_uv.y * u_h) > 0.6666667) c.rgb *= 0.624;\n"
    "  gl_FragColor = vec4(c.rgb * u_tint, c.a);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[ios] shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* One unit square with its texture coordinates. The shader stretches it over
 * u_rect, so a different destination is just a uniform. */
static const GLfloat kQuad[16] = {
    0, 0,  0, 0,
    1, 0,  1, 0,
    0, 1,  0, 1,
    1, 1,  1, 1
};

/* What a gesture looks like to a mouse. Kept in one struct because the rules are
 * fiddly and the messages they produce are exactly what the game sees. */
typedef struct {
    int    down;          /* the left button is held, by a finger */
    int    open;          /* a finger is on the glass */
    int    still;         /* no finger has travelled far */
    int    used;          /* the gesture already sent the right button */
    int    fingers;       /* the most fingers this gesture has used */
    int    single;        /* it has only ever been one finger */
    double at;            /* when it began */
    double last_tap;      /* for the double click */
    float  x, y;          /* where it began */
    float  tapx, tapy;    /* where the last tap was */
} HLEGesture;

@interface HLEGLView ()
@property (nonatomic, strong) NSTimer *press;
@property (nonatomic, strong) NSTimer *hud;
@end

@implementation HLEGLView {
    EAGLContext *_ctx;
    GLuint _fb, _rb, _prog, _buf, _tex, _cur;
    GLint _uRect, _uCrt, _uH, _uTint, _uTex;
    int _tex_w, _tex_h, _cur_w, _cur_h;
    unsigned _seq;
    UILabel *_hudLabel;
    HLEGesture _g;
}

+ (Class)layerClass { return [CAEAGLLayer class]; }

- (BOOL)setupGL {
    CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
    layer.opaque = YES;
    layer.drawableProperties = @{
        kEAGLDrawablePropertyRetainedBacking: [NSNumber numberWithBool:NO],
        kEAGLDrawablePropertyColorFormat:     kEAGLColorFormatRGBA8
    };

    _ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!_ctx || ![EAGLContext setCurrentContext:_ctx]) {
        fprintf(stderr, "[ios] no OpenGL ES 2.0 context\n");
        return NO;
    }
    fprintf(stderr, "[ios] %s / %s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));

    glGenFramebuffers(1, &_fb);
    glGenRenderbuffers(1, &_rb);
    glBindFramebuffer(GL_FRAMEBUFFER, _fb);
    glBindRenderbuffer(GL_RENDERBUFFER, _rb);
    if (![_ctx renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer]) {
        fprintf(stderr, "[ios] the layer has no drawable\n");
        return NO;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[ios] incomplete framebuffer: 0x%x\n", st);
        return NO;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return NO;
    _prog = glCreateProgram();
    glAttachShader(_prog, vs);
    glAttachShader(_prog, fs);
    glBindAttribLocation(_prog, 0, "a_pos");
    glBindAttribLocation(_prog, 1, "a_uv");
    glLinkProgram(_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(_prog, sizeof log, NULL, log);
        fprintf(stderr, "[ios] link: %s\n", log);
        return NO;
    }
    _uRect = glGetUniformLocation(_prog, "u_rect");
    _uCrt  = glGetUniformLocation(_prog, "u_crt");
    _uH    = glGetUniformLocation(_prog, "u_h");
    _uTint = glGetUniformLocation(_prog, "u_tint");
    _uTex  = glGetUniformLocation(_prog, "u_tex");

    glGenBuffers(1, &_buf);
    glBindBuffer(GL_ARRAY_BUFFER, _buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof kQuad, kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const GLvoid *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const GLvoid *)(2 * sizeof(GLfloat)));

    glGenTextures(1, &_tex);
    glGenTextures(1, &_cur);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return YES;
}

- (void)releaseGL {
    if (_ctx && [EAGLContext setCurrentContext:_ctx]) {
        glFinish();
        glDeleteFramebuffers(1, &_fb);
        glDeleteRenderbuffers(1, &_rb);
        glDeleteTextures(1, &_tex);
        glDeleteTextures(1, &_cur);
        glDeleteBuffers(1, &_buf);
        glDeleteProgram(_prog);
    }
    _fb = _rb = _prog = _buf = _tex = _cur = 0;
    _ctx = nil;
}

- (void)dealloc { [self releaseGL]; }

- (void)layoutSubviews {
    [super layoutSubviews];
    /* One game pixel is one point. A 1999 picture is 640x480; asking a Retina
     * screen for twice that buys nothing but the heat an A5 cannot spare, and
     * "sharp" scaling wants whole source pixels anyway. */
    if (self.contentScaleFactor != 1.0f) self.contentScaleFactor = 1.0f;
    CGSize b = self.bounds.size;
    int w = (int)lround(b.width), h = (int)lround(b.height);
    if (_ctx) glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    hle_ios_view_size(w, h);
}

- (CADisplayLink *)startPresenter {
    if (![self setupGL]) return nil;
    [self layoutSubviews];
    CADisplayLink *link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    link.frameInterval = 1;                     /* every vsync; the iOS 10 spelling is preferredFramesPerSecond */
    [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSDefaultRunLoopMode];
    return link;
}

- (void)tick:(CADisplayLink *)link { (void)link; [self render]; }

/* ---- uploading and drawing ---- */

/* Binds tex and makes sure its storage is w by h, reallocating only on a change
 * of size. Returns the name, so this can be wrapped around glBindTexture. */
static GLuint ensure_tex(GLuint tex, int *w_p, int *h_p, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    if (*w_p != w || *h_p != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        *w_p = w, *h_p = h;
    }
    return tex;
}

static void set_filter(GLenum filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

- (void)render {
    if (!_ctx || ![EAGLContext setCurrentContext:_ctx]) return;

    /* The guest publishes only when the game has drawn, so uploading only when
     * that counter moves is the difference between a copy per frame and one per
     * change. */
    unsigned seq = hle_ios_frame_seq();
    if (seq != _seq) {
        int fw = 0, fh = 0;
        const uint32_t *px = hle_ios_frame_acquire(&fw, &fh);
        if (px && fw > 0 && fh > 0) {
            ensure_tex(_tex, &_tex_w, &_tex_h, fw, fh);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            _seq = seq;
        }
    }

    int vw = (int)lround(self.bounds.size.width), vh = (int)lround(self.bounds.size.height);
    glBindFramebuffer(GL_FRAMEBUFFER, _fb);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (_tex_w <= 0 || _tex_h <= 0) goto present;

    int dx = 0, dy = 0, dw = 0, dh = 0;
    hle_ios_dst_rect(&dx, &dy, &dw, &dh);
    if (dw <= 0 || dh <= 0) goto present;

    int mode = hle_ios_scale_mode();
    glUseProgram(_prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(_uTex, 0);
    glUniform1f(_uH, (float)_tex_h);

    /* The bars: screen.c's dark blur of the picture beside a letterboxed one. A
     * second stretched draw of the same texture at 90/255 is all it takes. */
    if (dw < vw || dh < vh) {
        ensure_tex(_tex, &_tex_w, &_tex_h, _tex_w, _tex_h);
        set_filter(GL_LINEAR);
        glUniform1f(_uCrt, 0.0f);
        glUniform3f(_uTint, 90.0f / 255.0f, 90.0f / 255.0f, 90.0f / 255.0f);
        glUniform4f(_uRect, 0.0f, 0.0f, (float)vw, (float)vh);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* The picture. sharp and nearest want the source pixels kept, the rest read
     * better bilinear. screen.c's integer prescale needs a target texture and is
     * left out, so "sharp" here is simply nearest at the final size. */
    ensure_tex(_tex, &_tex_w, &_tex_h, _tex_w, _tex_h);
    set_filter((mode == 0 || mode == 3) ? GL_NEAREST : GL_LINEAR);
    glUniform1f(_uCrt, hle_ios_crt_on() ? 1.0f : 0.0f);
    glUniform3f(_uTint, 1.0f, 1.0f, 1.0f);
    glUniform4f(_uRect, (float)dx, (float)dy, (float)dw, (float)dh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* The guest's own cursor, over the picture: a touch screen has none. */
    int cw = 0, ch = 0, hotx = 0, hoty = 0, sx = 0, sy = 0;
    const uint32_t *cur = hle_ios_cursor_acquire(&cw, &ch, &hotx, &hoty);
    if (cur && cw > 0 && hle_ios_cursor_screen_pos(&sx, &sy)) {
        ensure_tex(_cur, &_cur_w, &_cur_h, cw, ch);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, cur);
        set_filter(GL_NEAREST);
        glUniform1f(_uCrt, 0.0f);
        glUniform3f(_uTint, 1.0f, 1.0f, 1.0f);
        glUniform4f(_uRect, (float)(sx - hotx), (float)(sy - hoty), (float)cw, (float)ch);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }

    glUseProgram(0);

present:
    glBindRenderbuffer(GL_RENDERBUFFER, _rb);
    [_ctx presentRenderbuffer:GL_RENDERBUFFER];
}

/* ---- a finger as a mouse ---- */

- (void)mouse:(int)kind at:(CGPoint)p {
    hle_ios_input_push(kind, (int)lround(p.x), (int)lround(p.y), 0, 0);
}

- (void)pressHidUsage:(int)usage down:(BOOL)down {
    int vk = 0, scan = 0;
    if (hle_hid_to_win(usage, &vk, &scan))
        hle_ios_input_push(down ? HLE_IOS_KEY_DOWN : HLE_IOS_KEY_UP, 0, 0, vk, scan);
}

- (NSUInteger)touchesIn:(UIEvent *)e { return [[e touchesForView:self] count]; }

- (CGPoint)where:(UIEvent *)e {
    for (UITouch *t in [e touchesForView:self])
        if (t.phase != UITouchPhaseEnded && t.phase != UITouchPhaseCancelled)
            return [t locationInView:self];
    return CGPointZero;
}

/* A finger that stays put is a right click once it has been still long enough. */
- (void)longPress:(NSTimer *)t {
    (void)t;
    if (!_g.down || !_g.still || !_g.single) return;
    [self.press invalidate];
    self.press = nil;
    CGPoint p = CGPointMake(_g.x, _g.y);
    _g.down = 0, _g.used = 1;
    [self mouse:HLE_IOS_MOUSE_UP at:p];
    [self mouse:HLE_IOS_MOUSE_RDOWN at:p];
    [self mouse:HLE_IOS_MOUSE_RUP at:p];
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches;
    NSUInteger n = [self touchesIn:event];
    CGPoint p = [self where:event];

    if (_g.open) {                                  /* a second or third finger */
        if (n > (NSUInteger)_g.fingers) _g.fingers = (int)n;
        _g.single = 0;
        if (n == 2 && !_g.used) {
            _g.used = 1;
            [self mouse:HLE_IOS_MOUSE_RDOWN at:p];
            [self mouse:HLE_IOS_MOUSE_RUP at:p];
        }
        return;
    }

    _g.open = _g.down = _g.still = _g.single = 1;
    _g.used = 0;
    _g.fingers = (int)n;
    _g.at = CACurrentMediaTime();
    _g.x = (float)p.x, _g.y = (float)p.y;
    [self mouse:HLE_IOS_MOUSE_DOWN at:p];
    [self mouse:HLE_IOS_MOUSE_MOVE at:p];
    [self.press invalidate];
    self.press = [NSTimer scheduledTimerWithTimeInterval:0.55 target:self
                                               selector:@selector(longPress:) userInfo:nil repeats:NO];
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches;
    if (!_g.open) return;
    CGPoint p = [self where:event];
    if (hypot(p.x - _g.x, p.y - _g.y) > 12.0) _g.still = 0;
    [self mouse:HLE_IOS_MOUSE_MOVE at:p];
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches; (void)event;
    int fingers = _g.fingers, still = _g.still, single = _g.single, used = _g.used;
    double began = _g.at;
    CGPoint p = CGPointMake(_g.x, _g.y);
    _g.open = 0, _g.fingers = 0;
    [self.press invalidate];
    self.press = nil;

    if (_g.down) {
        _g.down = 0;
        [self mouse:HLE_IOS_MOUSE_UP at:p];
    }
    if (fingers >= 3) {                             /* three fingers: the scaling mode */
        if (still) {
            hle_ios_scale_cycle();
            [self showHud:[NSString stringWithFormat:@"scaling: %s", hle_ios_scale_name()]];
        }
        return;
    }
    /* A tap is a press and a release in the same place; two of them close
     * together are the double click a menu with a spinner wants. */
    if (used || !single || !still || fingers != 1) return;
    double now = CACurrentMediaTime();
    if (now - began > 0.35) return;
    int twice = (now - _g.last_tap) < 0.4 &&
                hypot(p.x - _g.tapx, p.y - _g.tapy) < 24.0;
    _g.last_tap = twice ? 0 : now;
    _g.tapx = (float)p.x, _g.tapy = (float)p.y;
    [self mouse:HLE_IOS_MOUSE_DOWN at:p];
    [self mouse:HLE_IOS_MOUSE_UP at:p];
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches; (void)event;
    if (_g.down) [self mouse:HLE_IOS_MOUSE_UP at:CGPointMake(_g.x, _g.y)];
    _g.open = _g.down = 0, _g.fingers = 0;
    [self.press invalidate];
    self.press = nil;
}

/* ---- the label that says what just changed ---- */

- (void)showHud:(NSString *)text {
    if (!_hudLabel) {
        _hudLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        _hudLabel.textColor = [UIColor colorWithWhite:0.92f alpha:1.0f];
        _hudLabel.font = [UIFont boldSystemFontOfSize:15.0f];
        _hudLabel.textAlignment = NSTextAlignmentCenter;
        _hudLabel.userInteractionEnabled = NO;
        _hudLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
        [self addSubview:_hudLabel];
    }
    _hudLabel.text = [@"  " stringByAppendingString:[text stringByAppendingString:@"  "]];
    CGSize want = [_hudLabel sizeThatFits:CGSizeMake(400, 40)];
    CGSize view = self.bounds.size;
    _hudLabel.frame = CGRectMake((view.width - want.width) / 2, view.height - want.height - 28,
                                 want.width, want.height + 4);
    _hudLabel.backgroundColor = [UIColor colorWithWhite:0.0f alpha:0.55f];
    _hudLabel.alpha = 1.0f;
    [self.hud invalidate];
    self.hud = [NSTimer scheduledTimerWithTimeInterval:1.6 target:self
                                             selector:@selector(hudFaded:) userInfo:nil repeats:NO];
}

- (void)hudFaded:(NSTimer *)t {
    (void)t;
    [UIView animateWithDuration:0.4 animations:^{ self->_hudLabel.alpha = 0.0f; }];
}

@end
