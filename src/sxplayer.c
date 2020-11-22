#include <stdio.h>
#include <stdlib.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "sxplayer.h"

static const char *g_vertex_shader_data =
    "#version 330 core"                                  "\n"
    "layout(location = 0) in vec3 pos;"                  "\n"
    "layout(location = 1) in vec2 uv;"                   "\n"
    "out vec2 tex_coord;"                                "\n"
    "void main(){"                                       "\n"
    "    gl_Position = vec4(pos, 1);"                    "\n"
    "    tex_coord = uv;"                                "\n"
    "}"                                                  "\n";

static const char *g_fragment_shader_data =
    "#version 330 core"                                  "\n"
    "in vec2 tex_coord;"                                 "\n"
    "out vec3 color;"                                    "\n"
    "uniform sampler2D tex_sampler;"                     "\n"
    "void main() {"                                      "\n"
    "    color = texture(tex_sampler, tex_coord).rgb;"   "\n"
    "}"                                                  "\n";

static GLuint load_shader(const char *vertex_shader, const char *fragment_shader)
{
    char *info_log = NULL;
    int info_log_length = 0;

    GLint result = GL_FALSE;

    GLuint g_program_id = glCreateProgram();
    GLuint vertex_shader_id = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(vertex_shader_id, 1, &vertex_shader, NULL);
    glCompileShader(vertex_shader_id);

    glGetShaderiv(vertex_shader_id, GL_COMPILE_STATUS, &result);
    glGetShaderiv(vertex_shader_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 1) {
        info_log = malloc(info_log_length + 1);
        if (!info_log) {
            goto error;
        }
        glGetShaderInfoLog(vertex_shader_id, info_log_length, NULL, info_log);
        goto error;
    }

    glShaderSource(fragment_shader_id, 1, &fragment_shader, NULL);
    glCompileShader(fragment_shader_id);

    glGetShaderiv(fragment_shader_id, GL_COMPILE_STATUS, &result);
    glGetShaderiv(fragment_shader_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 1) {
        info_log = malloc(info_log_length + 1);
        if (!info_log) {
            goto error;
        }
        glGetShaderInfoLog(fragment_shader_id, info_log_length, NULL, info_log);
        goto error;
    }

    glAttachShader(g_program_id, vertex_shader_id);
    glAttachShader(g_program_id, fragment_shader_id);
    glLinkProgram(g_program_id);

    glGetProgramiv(g_program_id, GL_LINK_STATUS, &result);
    glGetProgramiv(g_program_id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (info_log_length > 1) {
        info_log = malloc(info_log_length + 1);
        if (!info_log) {
            goto error;
        }
        glGetProgramInfoLog(g_program_id, info_log_length, NULL, info_log);
        goto error;
    }

    glDeleteShader(vertex_shader_id);
    glDeleteShader(fragment_shader_id);

    return g_program_id;

error:
    if (info_log) {
        fprintf(stderr, "Failed to load shaders: %s\n", info_log);
        free(info_log);
    }

    if (vertex_shader_id) {
        glDeleteShader(vertex_shader_id);
    }

    if (fragment_shader_id) {
        glDeleteShader(fragment_shader_id);
    }

    if (g_program_id) {
        glDeleteProgram(g_program_id);
    }

    return 0;
}

static GLuint g_vertex_array_id;
static GLuint g_program_id;
static GLuint g_sampler_id;
static GLuint g_vertex_buffer_id;
static GLuint g_uv_buffer_id;
static GLuint g_texture_id;
static GLfloat g_padding;

static int g_paused;
static double g_last_rendered_frame_ts;
static double g_subtime;
struct sxplayer_info g_info;
struct sxplayer_ctx *g_s;

static int init(void)
{
    static const GLfloat vertex_buffer_data[] = {
        1.0f, -1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f,

        1.0f, -1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
    };
    static const GLfloat uv_buffer_data[] = {
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,

        1.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f
    };

    glClearColor(0.0f, 1.0f, 0.0f, 0.0f);

    g_program_id = load_shader(g_vertex_shader_data, g_fragment_shader_data);
    g_sampler_id = glGetUniformLocation(g_program_id, "tex_sampler");

    glGenVertexArrays(1, &g_vertex_array_id);
    glBindVertexArray(g_vertex_array_id);

    glGenTextures(1, &g_texture_id);
    glBindTexture(GL_TEXTURE_2D, g_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenBuffers(1, &g_vertex_buffer_id);
    glBindBuffer(GL_ARRAY_BUFFER, g_vertex_buffer_id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_buffer_data), vertex_buffer_data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenBuffers(1, &g_uv_buffer_id);
    glBindBuffer(GL_ARRAY_BUFFER, g_uv_buffer_id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(uv_buffer_data), uv_buffer_data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glUseProgram(g_program_id);

    return 0;
}

static int reset(void)
{
    glDeleteVertexArrays(1, &g_vertex_array_id);
    glDeleteProgram(g_program_id);
    glDeleteTextures(1, &g_sampler_id);
    glDeleteBuffers(1, &g_vertex_buffer_id);
    glDeleteBuffers(1, &g_uv_buffer_id);
    glDeleteTextures(1, &g_texture_id);

    return 0;
}

static int update_texture_padding(const float padding)
{
    const GLfloat uv_buffer_data[] = {
        padding, 1.0f,
        0.0f,    1.0f,
        0.0f,    0.0f,

        padding, 1.0f,
        0.0f,    0.0f,
        padding, 0.0f,
    };

    if (padding == g_padding)
        return 0;

    glGenBuffers(1, &g_uv_buffer_id);
    glBindBuffer(GL_ARRAY_BUFFER, g_uv_buffer_id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(uv_buffer_data), uv_buffer_data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    g_padding = padding;

    return 0;
}

static void error_callback(int error, const char *description)
{
    fprintf(stderr, "glfw error: %s\n", description);
}

static void render_frame(GLFWwindow *window, struct sxplayer_frame *frame)
{
    glClear(GL_COLOR_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture_id);
    if (frame) {
        if (frame->width != (frame->linesize >> 2)) {
            float padding = frame->width / (float)(frame->linesize >> 2);
            update_texture_padding(padding);
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame->linesize >> 2, frame->height, 0, GL_RGBA,  GL_UNSIGNED_BYTE, frame->data);
        g_last_rendered_frame_ts = frame->ts;
    }
    glUniform1i(g_sampler_id, 0);

    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, g_vertex_buffer_id);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, g_uv_buffer_id);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glDrawArrays(GL_TRIANGLES, 0, 2 * 3);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);

    glfwSwapBuffers(window);
}

static void render(GLFWwindow *window)
{
    const double time = glfwGetTime() - g_subtime;
    struct sxplayer_frame *frame = sxplayer_get_frame(g_s, time);

    render_frame(window, frame);

    sxplayer_release_frame(frame);
}

static double clipd(double v, double min, double max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static void seek_to(GLFWwindow *window, double t)
{
    t = clipd(t, 0, g_info.duration);
    printf("seek to %f/%f (%d%%)\n", t, g_info.duration, (int)(t/g_info.duration*100));
    g_subtime = glfwGetTime() - t;
    if (g_paused)
        render(window);
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);
        seek_to(window, xpos / g_info.width * g_info.duration);
    }
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            glfwSetWindowShouldClose(window, GL_TRUE);
        } else if (key == GLFW_KEY_SPACE) {
            g_paused ^= 1;
            if (g_paused) {
                printf("pause\n");
            } else {
                printf("unpause\n");
                g_subtime = glfwGetTime() - g_last_rendered_frame_ts;
            }
        } else if (key == GLFW_KEY_PERIOD || key == GLFW_KEY_S) {
            g_paused = 1;
            struct sxplayer_frame *frame = sxplayer_get_next_frame(g_s);
            if (frame) {
                printf("stepped to frame t=%f\n", frame->ts);
                render_frame(window, frame);
                sxplayer_release_frame(frame);
            }
        } else if (key == GLFW_KEY_LEFT) {
            seek_to(window, g_last_rendered_frame_ts - 5.0);
        } else if (key == GLFW_KEY_RIGHT) {
            seek_to(window, g_last_rendered_frame_ts + 5.0);
        }
    }
}

int main(int ac, char **av)
{
    int ret;
    GLFWwindow *window;

    if (ac != 2) {
        fprintf(stderr, "Usage: %s <media>\n", av[0]);
        return -1;
    }

    g_s = sxplayer_create(av[1]);
    if (!g_s)
        return -1;

    sxplayer_set_option(g_s, "sw_pix_fmt", SXPLAYER_PIXFMT_RGBA);
    sxplayer_set_option(g_s, "auto_hwaccel", 0);

    ret = sxplayer_get_info(g_s, &g_info);
    if (ret < 0)
        return ret;

    if (!glfwInit())
        return -1;

    glfwSetErrorCallback(error_callback);

    glfwWindowHint(GLFW_SAMPLES, 0);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(g_info.width, g_info.height, "sxplayer", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    glewExperimental = 1;
    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        return -1;
    }

    glfwSwapInterval(1);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    init();
    while (!glfwWindowShouldClose(window)) {
        if (!g_paused)
            render(window);
        glfwPollEvents();
    }
    reset();

    sxplayer_free(&g_s);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
