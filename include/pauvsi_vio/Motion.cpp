/*
 * vioMotion.cpp
 *
 *  Created on: Jan 16, 2017
 *      Author: kevin
 */


#include "vio.h"

void VIO::updateKeyFrameInfo()
{
	static KeyFrame candidate;

	if(keyFrames.size() < 1)
	{
		KeyFrame kf;
		kf.frame = &this->frameBuffer.at(frameBuffer.size() - 2);
		keyFrames.push_back(kf);
	}
	else
	{
		keyFrames.at(0).frame = &this->frameBuffer.at(frameBuffer.size() - 2);
	}
}


tf::Transform VIO::cameraTransformFromState(VIOState x, tf::Transform b2c)
{
	return tf::Transform(x.getTFQuaternion(), tf::Vector3(x.x(), x.y(), x.z())) * b2c;
}

void structureOnlyBundleAdjustment(Frame* cf, KeyFrame kf)
{
	g2o::SparseOptimizer optimizer; // this is the g2o optimizer which ultimately solves the problem
}






