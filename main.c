#include <stdio.h>
#include <GLFW/glfw3.h>
#include <GL/glu.h>

#include "sxplayer.h"

static int g_paused;
static double g_paused_at;
static double g_subtime;

static void error_callback(int error, const char *description)
{
    fprintf(stderr, "glfw error: %s\n", description);
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) {
            glfwSetWindowShouldClose(window, GL_TRUE);
        } else if (key == GLFW_KEY_SPACE) {
            g_paused ^= 1;
            if (g_paused)
                g_paused_at = glfwGetTime();
            else
                g_subtime += glfwGetTime() - g_paused_at;
        }
    }
}

static void render(GLFWwindow *window, struct sxplayer_ctx *s)
{
    if (g_paused)
        return;

    const double time = glfwGetTime() - g_subtime;
    struct sxplayer_frame *frame = sxplayer_get_frame(s, time);

    if (!frame)
        return;

    glEnable(GL_TEXTURE_2D);
    gluBuild2DMipmaps(GL_TEXTURE_2D, 3, frame->width, frame->height, GL_BGRA, GL_UNSIGNED_BYTE, frame->data);
    glBegin(GL_QUADS);
    glTexCoord2d(0.0, 0.0); glVertex2d(-1.0,  1.0);
    glTexCoord2d(1.0, 0.0); glVertex2d( 1.0,  1.0);
    glTexCoord2d(1.0, 1.0); glVertex2d( 1.0, -1.0);
    glTexCoord2d(0.0, 1.0); glVertex2d(-1.0, -1.0);
    glEnd();

    sxplayer_release_frame(frame);
}

int main(int ac, char **av)
{
    int ret;
    GLFWwindow *window;
    struct sxplayer_info info;
    struct sxplayer_ctx *s;

    if (ac != 2) {
        fprintf(stderr, "Usage: %s <media>\n", av[0]);
        return -1;
    }

    s = sxplayer_create(av[1]);
    if (!s)
        return -1;

    ret = sxplayer_get_info(s, &info);
    if (ret < 0)
        return ret;

    if (!glfwInit())
        return -1;

    glfwSetErrorCallback(error_callback);

    window = glfwCreateWindow(info.width, info.height, "sxplayer", NULL, NULL);
    if (!window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1);
    glfwSetKeyCallback(window, key_callback);

    while (!glfwWindowShouldClose(window)) {
        render(window, s);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    sxplayer_free(&s);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
