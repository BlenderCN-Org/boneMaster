/* Float Math */

float safe_divide(float a, float b)
{
  return (b != 0.0) ? a / b : 0.0;
}

/* Modulo with C sign convention. mod in GLSL will take absolute for negative numbers. */
float c_mod(float a, float b)
{
  return (b != 0.0 && a != b) ? sign(a) * mod(abs(a), b) : 0.0;
}

float compatible_pow(float x, float y)
{
  if (y == 0.0) { /* x^0 -> 1, including 0^0 */
    return 1.0;
  }

  /* glsl pow doesn't accept negative x */
  if (x < 0.0) {
    if (mod(-y, 2.0) == 0.0) {
      return pow(-x, y);
    }
    else {
      return -pow(-x, y);
    }
  }
  else if (x == 0.0) {
    return 0.0;
  }

  return pow(x, y);
}

float hypot(float x, float y)
{
  return sqrt(x * x + y * y);
}

int floor_to_int(float x)
{
  return int(floor(x));
}

int quick_floor(float x)
{
  return int(x) - ((x < 0) ? 1 : 0);
}

float floorfrac(float x, out int i)
{
  float x_floor = floor(x);
  i = int(x_floor);
  return x - x_floor;
}

/* Vector Math */

vec3 safe_divide(vec3 a, vec3 b)
{
  return vec3(safe_divide(a.x, b.x), safe_divide(a.y, b.y), safe_divide(a.z, b.z));
}

vec3 c_mod(vec3 a, vec3 b)
{
  return vec3(c_mod(a.x, b.x), c_mod(a.y, b.y), c_mod(a.z, b.z));
}

void vector_mix(float strength, vec3 a, vec3 b, out vec3 outVector)
{
  outVector = strength * a + (1 - strength) * b;
}

void invert_z(vec3 v, out vec3 outv)
{
  v.z = -v.z;
  outv = v;
}

void vector_normalize(vec3 normal, out vec3 outnormal)
{
  outnormal = normalize(normal);
}

/* Matirx Math */

void direction_transform_m4v3(vec3 vin, mat4 mat, out vec3 vout)
{
  vout = (mat * vec4(vin, 0.0)).xyz;
}

void normal_transform_transposed_m4v3(vec3 vin, mat4 mat, out vec3 vout)
{
  vout = transpose(mat3(mat)) * vin;
}

void point_transform_m4v3(vec3 vin, mat4 mat, out vec3 vout)
{
  vout = (mat * vec4(vin, 1.0)).xyz;
}
