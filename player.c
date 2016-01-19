#include <stdio.h>
#include <GLFW/glfw3.h>
#include <GL/glu.h>

#include "sxplayer.h"

static int g_paused;
static double g_last_rendered_frame_ts;
static double g_subtime;
struct sxplayer_info g_info;
struct sxplayer_ctx *g_s;

static void error_callback(int error, const char *description)
{
    fprintf(stderr, "glfw error: %s\n", description);
}

static void show_frame(const struct sxplayer_frame *frame)
{
    gluBuild2DMipmaps(GL_TEXTURE_2D, 3, frame->width, frame->height, GL_BGRA, GL_UNSIGNED_BYTE, frame->data);
    glBegin(GL_QUADS);
    glTexCoord2d(0.0, 0.0); glVertex2d(-1.0,  1.0);
    glTexCoord2d(1.0, 0.0); glVertex2d( 1.0,  1.0);
    glTexCoord2d(1.0, 1.0); glVertex2d( 1.0, -1.0);
    glTexCoord2d(0.0, 1.0); glVertex2d(-1.0, -1.0);
    glEnd();
    g_last_rendered_frame_ts = frame->ts;
}

static void render(GLFWwindow *window)
{
    const double time = glfwGetTime() - g_subtime;
    struct sxplayer_frame *frame = sxplayer_get_frame(g_s, time);

    if (!frame)
        return;
    show_frame(frame);
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
                show_frame(frame);
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

    ret = sxplayer_get_info(g_s, &g_info);
    if (ret < 0)
        return ret;

    if (!glfwInit())
        return -1;

    glfwSetErrorCallback(error_callback);

    window = glfwCreateWindow(g_info.width, g_info.height, "sxplayer", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);

    glEnable(GL_TEXTURE_2D);

    while (!glfwWindowShouldClose(window)) {
        if (!g_paused)
            render(window);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    sxplayer_free(&g_s);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
