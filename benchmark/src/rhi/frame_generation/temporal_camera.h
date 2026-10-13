#ifndef HEAVEN_TEMPORAL_CAMERA_H
#define HEAVEN_TEMPORAL_CAMERA_H

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace heaven {

// The public Camera::getProjectionMatrix returns its original GL [-1,1]
// projection (Camera.cpp forwards to getUserProjectionMatrix). Only the
// private FCamera::getProjectionMatrix converts to reversed [0,1] depth.
// Public scaling/shift are separate, so reproduce their render-time image
// transform explicitly when supplied. Filament stores twice the public shift.
template<class Matrix>
glm::mat4 temporal_gl_projection(const Matrix& user_projection,
        glm::vec2 scaling = {1.0f,1.0f}, glm::vec2 shift = {0.0f,0.0f}) {
    glm::mat4 projection;
    for(int column=0;column<4;++column) {
        for(int row=0;row<4;++row)
            projection[column][row]=static_cast<float>(user_projection[column][row]);
        projection[column][0]=projection[column][0]*scaling.x+2.0f*shift.x*projection[column][3];
        projection[column][1]=projection[column][1]*scaling.y+2.0f*shift.y*projection[column][3];
    }
    return projection;
}

}

#endif // HEAVEN_TEMPORAL_CAMERA_H
