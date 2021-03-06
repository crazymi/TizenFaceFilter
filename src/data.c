/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "main.h"
#include "data.h"
#include <stdio.h>
#include <unistd.h>
#include <camera.h>
#include <storage.h>
#include <pthread.h>

#define BUFLEN 512

typedef struct _camdata {
    camera_h g_camera; /* Camera handle */
    Evas_Object *cam_display;
    Evas_Object *cam_display_box;
    Evas_Object *display;
    Evas_Object *preview_bt;
    Evas_Object *face_bt;
    Evas_Object *photo_bt;
    bool cam_prev;
    bool face_running;
} camdata;
static camdata cam_data;

static char *camera_directory = NULL;

pthread_mutex_t facelock;

camera_detected_face_s *cam_face;
int cam_face_num = 0;

/**
 * @brief Maps the given camera state to its string representation.
 *
 * @param state  The camera state that should be mapped to its literal
 *               representation
 *
 * @return The string representation of the given camera state
 */
static const char *_camera_state_to_string(camera_state_e state)
{
    switch (state) {
    case CAMERA_STATE_NONE:
        return "CAMERA_STATE_NONE";

    case CAMERA_STATE_CREATED:
        return "CAMERA_STATE_CREATED";

    case CAMERA_STATE_PREVIEW:
        return "CAMERA_STATE_PREVIEW";

    case CAMERA_STATE_CAPTURING:
        return "CAMERA_STATE_CAPTURING";

    case CAMERA_STATE_CAPTURED:
        return "CAMERA_STATE_CAPTURED";

    default:
        return "Unknown";
    }
}

/**
 * @brief Gets the ID of the internal storage.
 * @details It assigns the get ID to the variable passed as the user data
 *          to the callback. This callback is called for every storage supported
 *          by the device.
 * @remarks This function matches the storage_device_supported_cb() signature
 *          defined in the storage-expand.h header file.
 *
 * @param storage_id  The unique ID of the detected storage
 * @param type        The type of the detected storage
 * @param state       The current state of the detected storage.
 *                    This argument is not used in this case.
 * @param path        The absolute path to the root directory of the detected
 *                    storage. This argument is not used in this case.
 * @param user_data   The user data passed via void pointer
 *
 * @return @c true to continue iterating over supported storages, @c false to
 *         stop the iteration.
 */
static bool _storage_cb(int storage_id, storage_type_e type,
                        storage_state_e state, const char *path,
                        void *user_data)
{
    if (STORAGE_TYPE_INTERNAL == type) {
        int *internal_storage_id = (int *) user_data;

        if (NULL != internal_storage_id)
            *internal_storage_id = storage_id;

        /* Internal storage found, stop the iteration. */
        return false;
    } else {
        /* Continue iterating over storages. */
        return true;
    }
}

/**
 * @brief Retrieves all supported camera preview resolutions.
 * @details Called for every preview resolution that is supported by the device.
 * @remarks This function matches the camera_supported_preview_resolution_cb()
 *          signature defined in the camera.h header file.
 *
 * @param width       The preview image width
 * @param height      The preview image height
 * @param user_data   The user data passed from
 *                    the camera_supported_preview_resolution_cb() function
 *
 * @return @c true to continue with the next iteration of the loop,
 *         otherwise @c false to break out of the loop
 */
static bool _preview_resolution_cb(int width, int height, void *user_data)
{
    if (NULL != user_data && width < 700) {
        int *resolution = (int *) user_data;
        resolution[0] = width;
        resolution[1] = height;
    }

    return true;
}

/**
 * @brief Called to get the information about image data taken by the camera
 *        once per frame while capturing.
 * @details Called when image capturing is finished.
 * @remarks This function matches the camera_capture_completed_cb() signature
 *          defined in the camera.h header file.
 *
 * @param user_data  The user data passed from the callback registration
 *                   function. This argument is not used in this case.
 */
static void _camera_completed_cb(void *user_data)
{
    /* Start the camera preview again. */
    int error_code = camera_start_preview(cam_data.g_camera);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_start_preview", error_code);
        PRINT_MSG("Could not restart the camera preview.");
    }

    /*
     * The following actions (start -> stop -> start preview) are required
     * to deal with a bug related to the camera brightness changes
     * (Without applying this workaround, after taking a photo,
     * the changes of the camera preview brightness are not visible).
     */
    error_code = camera_stop_preview(cam_data.g_camera);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_stop_preview", error_code);
        PRINT_MSG("Could not stop the camera preview.");
    }

    error_code = camera_start_preview(cam_data.g_camera);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_start_preview", error_code);
        PRINT_MSG("Could not restart the camera preview.");
    }

    /*
     * The following actions (start -> stop -> start preview) are required
     * to deal with a bug related to the camera brightness changes
     * (without applying this workaround, after taking a photo,
     * the changes of the camera preview brightness are not visible).
     */
    error_code = camera_stop_preview(cam_data.g_camera);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_stop_preview", error_code);
        PRINT_MSG("Could not stop the camera preview.");
    }

    error_code = camera_start_preview(cam_data.g_camera);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_start_preview", error_code);
        PRINT_MSG("Could not restart the camera preview.");
    }
}

/**
 * @brief Called when the image is saved.
 * @remarks This function matches the Ecore_Cb() signature defined in the
 *          Ecore_Legacy.h header file.
 *
 * @param data  The user data passed via void pointer
 */
static void _image_saved(void *data)
{
    PRINT_MSG("Image stored in the %s", (char *) data);
    free(data);
}

/**
 * @brief Called to get information about image data taken by the camera
 *        once per frame while capturing.
 * @remarks This function matches the camera_capturing_cb() signature defined in
 *          the camera.h header file.
 *
 * @param image      The image data of the captured picture
 * @param postview   The image data of the post view. This argument is not used
 *                   in this case.
 * @param thumbnail  The image data of the thumbnail (it should be @c NULL if
 *                   the available thumbnail data does not exist). This argument
 *                   is not used in this case.
 * @param user_data  The user data passed from the callback registration
 *                   function. This argument is not used in this case.
 */
static void _camera_capturing_cb(camera_image_data_s *image,
                                 camera_image_data_s *postview,
                                 camera_image_data_s *thumbnail,
                                 void *user_data)
{
    if (NULL != image && NULL != image->data) {
        dlog_print(DLOG_DEBUG, LOG_TAG, "Writing image to file.");

        char *file_path = (char *) malloc(sizeof(char) * BUFLEN);

        /* Create a full path to newly created file for storing the taken photo. */
        snprintf(file_path, BUFLEN, "%s/cam%d.jpg", camera_directory,
                (int) time(NULL));

        /* Open the file for writing. */
        FILE *file = fopen(file_path, "w+");

        /* Write the image to a file. */
        fwrite(image->data, 1, image->size, file);

        /* Close the file. */
        fclose(file);

        ecore_job_add(_image_saved, (void *) file_path);
    } else {
        dlog_print(DLOG_ERROR, LOG_TAG,
                "An error occurred during taking the photo. The image is NULL.");
    }
}

/**
 * @brief Checks the camera focus state.
 * @details Callback function invoked when the camera focusing state changes.
 * @remarks This function matches the camera_focus_changed_cb() signature
 *          defined in the camera.h header file.
 *
 * @param state      The current state of the auto-focus
 * @param user_data  The user data passed from the callback registration
 *                   function. This argument is not used in this case.
 */
static void _camera_focus_cb(camera_focus_state_e state, void *user_data)
{
    if (CAMERA_FOCUS_STATE_FOCUSED == state) {
        /* Take a photo. */
        int error_code = camera_start_capture(cam_data.g_camera,
                _camera_capturing_cb, _camera_completed_cb,
                NULL);
        if (CAMERA_ERROR_NONE != error_code) {
            DLOG_PRINT_ERROR("camera_start_capture", error_code);
            PRINT_MSG("Could not start taking a photo.");
        }
    }
}

/**
 * @brief Takes a photo.
 * @details Called when the "Take a photo" button is clicked.
 * @remarks This function matches the Evas_Smart_Cb() signature defined in the
 *          Evas_Legacy.h header file.
 *
 * @param data        The user data passed via void pointer. This argument is
 *                    not used in this case.
 * @param obj         A handle to the object on which the event occurred. In
 *                    this case it's a pointer to the button object. This
 *                    argument is not used in this case.
 * @param event_info  A pointer to a data which is totally dependent on the
 *                    smart object's implementation and semantic for the given
 *                    event. This argument is not used in this case.
 */
static void __camera_cb_photo(void *data, Evas_Object *obj, void *event_info)
{
    /* Focus the camera on the current view. */
    int error_code = camera_start_focusing(cam_data.g_camera, false);
    if (CAMERA_ERROR_NONE != error_code) {
        if (CAMERA_ERROR_NOT_SUPPORTED != error_code) {
            DLOG_PRINT_ERROR("camera_start_focusing", error_code);
            PRINT_MSG(
                    "Focusing is not supported on this device."
                    " The picture will be taken without focusing.");
        } else {
            dlog_print(DLOG_INFO, LOG_TAG,
                    "Focusing is not supported on this device."
                    " The picture will be taken without focusing.");
        }

        /*
         * Take a photo (If the focusing is not supported, then just take a
         * photo, without focusing).
         */
        error_code = camera_start_capture(cam_data.g_camera,
                _camera_capturing_cb, _camera_completed_cb,
                NULL);
        if (CAMERA_ERROR_NONE != error_code) {
            DLOG_PRINT_ERROR("camera_start_capture", error_code);
            PRINT_MSG("Could not start capturing the photo.");
        }
    }
}

static void __camera_face_detected_cb(camera_detected_face_s *faces, int count, void *user_data)
{
	if(count == 0){
		cam_face_num = count;
		return;
	}

	if(count > 0 && faces != NULL){
		// as there are only MAXIMUM_FACE_NUMBER space allocated for cam_face
		count = (count > MAXIMUM_FACE_NUMBER) ? MAXIMUM_FACE_NUMBER : count;

		if(pthread_mutex_trylock(&facelock) == 0){
			memcpy(cam_face, faces, sizeof(camera_detected_face_s)*count);
			cam_face_num = count;
			PRINT_MSG("detected: (%d, %d)", faces->x, faces->y);
			pthread_mutex_unlock(&facelock);
		}
	}
}

static void __camera_cb_face(void *data, Evas_Object *obj, void *event_info)
{
	int error_code = 0;
	if(cam_data.face_running){
		error_code = camera_stop_face_detection(cam_data.g_camera);
		if(error_code != CAMERA_ERROR_NONE){
			DLOG_PRINT_ERROR("camera_stop_face_detection", error_code);
			PRINT_MSG("Fail to stop face detection");
		} else {
			cam_data.face_running = false;
		}
	} else {
		error_code = camera_start_face_detection(cam_data.g_camera, __camera_face_detected_cb, NULL);
		if(error_code != CAMERA_ERROR_NONE){
			DLOG_PRINT_ERROR("camera_start_face_detection", error_code);
			PRINT_MSG("Fail to start face detection");
		} else {
			cam_data.face_running = true;
		}
	}
}

static void __camera_preview_cb(camera_preview_data_s *frame, void *user_data)
{
	if(pthread_mutex_trylock(&facelock) == 0){
		if(cam_face != NULL && cam_face_num > 0 && cam_data.face_running){
			int begin = cam_face->x + cam_face->y*640;
			for(int j=0;j<cam_face->height;j++){
				for(int i=0;i<cam_face->width;i++){
					int end = begin + i + j*640;
					frame->data.double_plane.y[end] = 0;
				}
			}
		}
		pthread_mutex_unlock(&facelock);
	}
}

/**
 * @brief Starts the camera preview.
 * @details Called when the "Start preview" button is clicked.
 * @remarks This function matches the Evas_Smart_Cb() signature defined in the
 *          Evas_Legacy.h header file.
 *
 * @param data        The user data passed via void pointer. This argument is
 *                    not used in this case.
 * @param obj         A handle to the object on which the event occurred. In
 *                    this case it's a pointer to the button object. This
 *                    argument is not used in this case.
 * @param event_info  A pointer to a data which is totally dependent on the
 *                    smart object's implementation and semantic for the given
 *                    event. This argument is not used in this case.
 */
static void __camera_cb_preview(void *data, Evas_Object *obj,
                                void *event_info)
{
    int error_code = CAMERA_ERROR_NONE;

    if (!cam_data.cam_prev) {
        /* Show the camera preview UI element. */
        evas_object_size_hint_weight_set(cam_data.display, EVAS_HINT_EXPAND,
                2.0);
        evas_object_size_hint_weight_set(cam_data.cam_display_box,
        		EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
        evas_object_show(cam_data.cam_display_box);

        /* Start the camera preview. */
        error_code = camera_start_preview(cam_data.g_camera);
        if (CAMERA_ERROR_NONE != error_code) {
            DLOG_PRINT_ERROR("camera_start_preview", error_code);
            PRINT_MSG("Could not start the camera preview.");
            return;
        }

        /* Set preview callback */
		error_code = camera_set_preview_cb(cam_data.g_camera, __camera_preview_cb, NULL);
		if(error_code != CAMERA_ERROR_NONE){
			DLOG_PRINT_ERROR("camera_set_preview_cb", error_code);
			PRINT_MSG("Could not set preview callback.");
			return;
		}

        PRINT_MSG("Camera preview started.");
        cam_data.cam_prev = true;

        elm_object_text_set(cam_data.preview_bt, "Stop preview");

        /* Enable other camera buttons. */
        elm_object_disabled_set(cam_data.face_bt, EINA_FALSE);
        // elm_object_disabled_set(cam_data.photo_bt, EINA_FALSE);
    } else {
        /* Hide the camera preview UI element. */
        evas_object_size_hint_weight_set(cam_data.display, EVAS_HINT_EXPAND,
                0.0);
        evas_object_size_hint_weight_set(cam_data.cam_display_box,
                EVAS_HINT_EXPAND, 0.0);
        evas_object_hide(cam_data.cam_display_box);

        /* unset the camera preview callback */
        error_code = camera_unset_preview_cb(cam_data.g_camera);
		if (CAMERA_ERROR_NONE != error_code) {
			DLOG_PRINT_ERROR("camera_unset_preview_cb", error_code);
			PRINT_MSG("Could not unset the camera preview callback.");
			return;
		}

        /* Stop the camera preview. */
        error_code = camera_stop_preview(cam_data.g_camera);
        if (CAMERA_ERROR_NONE != error_code) {
            DLOG_PRINT_ERROR("camera_stop_preview", error_code);
            PRINT_MSG("Could not stop the camera preview.");
            return;
        }

        PRINT_MSG("Camera preview stopped.");
        cam_data.cam_prev = false;
        cam_data.face_running = false;

        elm_object_text_set(cam_data.preview_bt, "Start preview");

        /* Disable other camera buttons. */
        elm_object_disabled_set(cam_data.face_bt, EINA_TRUE);
        // elm_object_disabled_set(cam_data.photo_bt, EINA_TRUE);
    }
}

/**
 * @brief Called when the "Camera" screen is being closed.
 */
void camera_pop_cb()
{
    /* Stop camera focusing. */
    camera_cancel_focusing(cam_data.g_camera);

    /* Stop camera preview. */
    camera_stop_preview(cam_data.g_camera);
    cam_data.cam_prev = false;

    /* Unregister camera preview callback. */
    camera_unset_preview_cb(cam_data.g_camera);

    /* Unregister camera focus change callback. */
    camera_unset_focus_changed_cb(cam_data.g_camera);

    /* Destroy camera handle. */
    camera_destroy(cam_data.g_camera);
    cam_data.g_camera = NULL;

    /* Free the Camera directory path. */
    free(camera_directory);
}

/**
 * @brief Called when the camera preview display is being resized.
 * @details It resizes the camera preview to fit the camera preview display.
 * @remarks This function matches the Evas_Object_Event_Cb() signature defined
 *          in the Evas_Legacy.h header file.
 *
 * @param data        The user data passed via void pointer. In this case it is
 *                    used for passing the camera preview object. This argument
 *                    is not used in this case.
 * @param e           The canvas pointer on which the event occurred
 * @param obj         A pointer to the object on which the event occurred. In
 *                    this case it is the camera preview display.
 * @param event_info  In case of the EVAS_CALLBACK_RESIZE event, this parameter
 *                    is NULL. This argument is not used in this case.
 */
void _post_render_cb(void *data, Evas *e, Evas_Object *obj, void *event_info)
{
    Evas_Object **cam_data_image = (Evas_Object **) data;

    /* Get the size of the parent box. */
    int x = 0, y = 0, w = 0, h = 0;
    evas_object_geometry_get(obj, &x, &y, &w, &h);

    /* Set the size of the image object. */
    evas_object_resize(*cam_data_image, w, h);
    evas_object_move(*cam_data_image, 0, y);
}

/**
 * @brief Creates the main view of the application.
 *
 */
void create_buttons_in_main_window(void)
{
    /*
     * Create the window with camera preview and buttons for manipulating the
     * camera and taking the photo.
     */
    cam_data.display = _create_new_cd_display("Camera", NULL);

    /* Create a box for the camera preview. */
    cam_data.cam_display_box = elm_box_add(cam_data.display);
    elm_box_horizontal_set(cam_data.cam_display_box, EINA_FALSE);
    evas_object_size_hint_align_set(cam_data.cam_display_box, EVAS_HINT_FILL,
            EVAS_HINT_FILL);
    evas_object_size_hint_weight_set(cam_data.cam_display_box, EVAS_HINT_EXPAND,
            EVAS_HINT_EXPAND);
    elm_box_pack_end(cam_data.display, cam_data.cam_display_box);
    evas_object_show(cam_data.cam_display_box);

    Evas *evas = evas_object_evas_get(cam_data.cam_display_box);
    cam_data.cam_display = evas_object_image_add(evas);
    int a= 0;
    evas_object_event_callback_add(cam_data.cam_display_box,
            EVAS_CALLBACK_RESIZE, _post_render_cb, &(cam_data.cam_display));

    /* Create buttons for the Camera. */
    cam_data.preview_bt = _new_button(cam_data.display, "Start preview",
            __camera_cb_preview);
    cam_data.face_bt = _new_button(cam_data.display, "Face Detect",
                __camera_cb_face);
    // cam_data.photo_bt = _new_button(cam_data.display, "Take a photo", __camera_cb_photo);

    /*
     * Disable buttons different than "Start preview" when the preview is not
     * running.
     */
    elm_object_disabled_set(cam_data.face_bt, EINA_TRUE);
    // elm_object_disabled_set(cam_data.photo_bt, EINA_TRUE);

    /* Create the camera handle for the main camera of the device. */
    int error_code = camera_create(CAMERA_DEVICE_CAMERA1, &(cam_data.g_camera));
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_create", error_code);
        PRINT_MSG("Could not create a handle to the camera.");
        return;
    }

    /* Check the camera state after creating the handle. */
    camera_state_e state;
    error_code = camera_get_state(cam_data.g_camera, &state);
    if (CAMERA_ERROR_NONE != error_code || CAMERA_STATE_CREATED != state) {
        dlog_print(DLOG_ERROR, LOG_TAG,
                "camera_get_state() failed! Error code = %d, state = %s",
                error_code, _camera_state_to_string(state));
        return;
    }

    /*
     * Enable EXIF data storing during taking picture. This is required to edit
     * the orientation of the image.
     */
    error_code = camera_attr_enable_tag(cam_data.g_camera, true);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_attr_enable_tag", error_code);
        PRINT_MSG("Could not enable the camera tag.");
    }

    /*
     * Set the camera image orientation. Required (on Kiran device) to save the
     * image in regular orientation (without any rotation).
     */
    error_code = camera_attr_set_tag_orientation(cam_data.g_camera,
            CAMERA_ATTR_TAG_ORIENTATION_RIGHT_TOP);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_attr_set_tag_orientation", error_code);
        PRINT_MSG("Could not set the camera image orientation.");
    }

    /* Set the picture quality attribute of the camera to maximum. */
    error_code = camera_attr_set_image_quality(cam_data.g_camera, 100);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_attr_set_image_quality", error_code);
        PRINT_MSG("Could not set the picture quality.");
    }

    /* Set the display for the camera preview. */
    error_code = camera_set_display(cam_data.g_camera, CAMERA_DISPLAY_TYPE_EVAS,
            GET_DISPLAY(cam_data.cam_display));
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_set_display", error_code);
        PRINT_MSG("Could not set the camera display.");
        return;
    }

    /* Set the resolution of the camera preview: */
    int resolution[2];

    /* 1. Find the best resolution that is supported by the device. */
    error_code = camera_foreach_supported_preview_resolution(cam_data.g_camera,
            _preview_resolution_cb, resolution);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_foreach_supported_preview_resolution",
                error_code);
        PRINT_MSG("Could not find the best resolution for the camera preview.");
        return;
    }

    /* 2. Set found supported resolution for the camera preview. */
    error_code = camera_set_preview_resolution(cam_data.g_camera, resolution[0],
            resolution[1]);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_set_preview_resolution", error_code);
        PRINT_MSG("Could not set the camera preview resolution.");
    } else
        PRINT_MSG("Camera resolution set to: %d %d", resolution[0],
                resolution[1]);

    /* Set the capture format for the camera. */
    error_code = camera_set_capture_format(cam_data.g_camera,
            CAMERA_PIXEL_FORMAT_JPEG);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_set_capture_format", error_code);
        PRINT_MSG("Could not set the capturing format.");
    }

    /* Set the focusing callback function. */
    error_code = camera_set_focus_changed_cb(cam_data.g_camera,
            _camera_focus_cb, NULL);
    if (CAMERA_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("camera_set_focus_changed_cb", error_code);
        PRINT_MSG("Could not set a callback for the focus changes.");
    }

    /* Set preview callback */
    error_code = camera_set_preview_cb(cam_data.g_camera, __camera_preview_cb, NULL);
    if(error_code != CAMERA_ERROR_NONE){
    	DLOG_PRINT_ERROR("camera_set_preview_cb", error_code);
		PRINT_MSG("Could not set preview callback.");
		return;
    }

    /* set face detection */
    cam_data.face_running = false;

	if(camera_is_supported_face_detection(cam_data.g_camera)){
		PRINT_MSG("face support");
		cam_face = (camera_detected_face_s*)malloc(sizeof(camera_detected_face_s)*MAXIMUM_FACE_NUMBER);
	} else {
		PRINT_MSG("face NO support");
	}


    /* Get the path to the Camera directory: */

    /* 1. Get internal storage id. */
    int internal_storage_id = -1;

    error_code = storage_foreach_device_supported(_storage_cb,
            &internal_storage_id);
    if (STORAGE_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("storage_foreach_device_supported", error_code);
        PRINT_MSG("Could not get internal storage id.");
        return;
    }

    /* 2. Get the path to the Camera directory. */
    error_code = storage_get_directory(internal_storage_id,
            STORAGE_DIRECTORY_CAMERA, &camera_directory);
    if (STORAGE_ERROR_NONE != error_code) {
        DLOG_PRINT_ERROR("storage_get_directory", error_code);
        PRINT_MSG("Could not get the path to the Camera directory.");
    }

    if(pthread_mutex_init(&facelock, NULL) != 0){
    	PRINT_MSG("Fail to initiate mutex.");
    }

}
