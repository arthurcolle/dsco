#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/*
 * Mobius strip rendered as a spinning donut-style ASCII animation.
 *
 * Parametrize the strip:
 *   u in [0, 2pi)   -> angle around the main ring
 *   v in [-1, 1]    -> position across the half-width of the strip
 *
 *   P(u,v) = ( (R + v*cos(u/2)) * cos(u),
 *              (R + v*cos(u/2)) * sin(u),
 *               v * sin(u/2) )
 *
 * The cross-section direction rotates by u/2 -> a single half-twist over
 * one loop, which is exactly what makes the surface one-sided.
 */

#define W 80
#define H 24

static const char *shades = ".,-~:;=!*#$@";

int main(void) {
    char  buf[W * H];
    float zbuf[W * H];

    const float R  = 2.0f;     /* ring radius        */
    const float K2 = 6.0f;     /* viewer distance    */
    const float K1 = W * K2 * 3.0f / (8.0f * (R + 1.0f)); /* projection scale */

    float A = 0.0f, B = 0.0f;  /* tumble angles */

    for (;;) {
        memset(buf, ' ', sizeof buf);
        for (int i = 0; i < W * H; i++) zbuf[i] = 0.0f;

        float cA = cosf(A), sA = sinf(A);
        float cB = cosf(B), sB = sinf(B);

        /* sweep around the ring */
        for (float u = 0.0f; u < 2.0f * (float)M_PI; u += 0.02f) {
            float cu = cosf(u), su = sinf(u);
            float ch = cosf(u * 0.5f), sh = sinf(u * 0.5f); /* half-twist */

            /* sweep across the strip width */
            for (float v = -1.0f; v <= 1.0f; v += 0.05f) {
                float rad = R + v * ch;

                /* point on the surface */
                float px = rad * cu;
                float py = rad * su;
                float pz = v * sh;

                /* surface normal via partial derivatives Pu x Pv */
                /* Pv = d/dv = (ch*cu, ch*su, sh) */
                float pvx = ch * cu, pvy = ch * su, pvz = sh;
                /* Pu = d/du */
                float dch = -0.5f * sh;            /* d(ch)/du */
                float drad = v * dch;              /* d(rad)/du */
                float pux = drad * cu - rad * su;
                float puy = drad * su + rad * cu;
                float puz = v * 0.5f * ch;         /* d(v*sh)/du */
                /* n = Pu x Pv */
                float nx = puy * pvz - puz * pvy;
                float ny = puz * pvx - pux * pvz;
                float nz = pux * pvy - puy * pvx;
                float nl = sqrtf(nx*nx + ny*ny + nz*nz) + 1e-6f;
                nx /= nl; ny /= nl; nz /= nl;

                /* rotate point: Rx(A) then Ry(B) */
                float x1 = px,            y1 = py * cA - pz * sA, z1 = py * sA + pz * cA;
                float x  = x1 * cB + z1 * sB, y = y1,            z = -x1 * sB + z1 * cB;
                /* rotate normal the same way */
                float n1x = nx,             n1y = ny * cA - nz * sA, n1z = ny * sA + nz * cA;
                float Nx  = n1x * cB + n1z * sB, Ny = n1y,           Nz = -n1x * sB + n1z * cB;

                float ooz = 1.0f / (z + K2);
                int sx = (int)(W / 2 + K1 * ooz * x);
                int sy = (int)(H / 2 - K1 * ooz * y * 0.5f); /* aspect squash */

                if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
                int o = sx + W * sy;
                if (ooz <= zbuf[o]) continue;

                /* lighting: dot normal with light dir (0, 1, -1)/sqrt2 */
                float L = (Ny - Nz) * 0.7071f;
                if (L < 0) L = -L;          /* both sides lit (one-sided surface) */
                int lum = (int)(L * 11.0f);
                if (lum < 0) lum = 0; if (lum > 11) lum = 11;

                zbuf[o] = ooz;
                buf[o]  = shades[lum];
            }
        }

        /* draw frame */
        printf("\x1b[H");
        for (int y = 0; y < H; y++) {
            fwrite(buf + y * W, 1, W, stdout);
            putchar('\n');
        }
        fflush(stdout);

        A += 0.04f;
        B += 0.02f;
        usleep(30000);
    }
    return 0;
}
