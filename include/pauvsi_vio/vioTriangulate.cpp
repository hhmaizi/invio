/*
 * vioTriangulate.cpp
 *
 *  Created on: Jan 16, 2017
 *      Author: kevin
 */

#include "vio.h"

struct less_than_key {
	inline bool operator()(const VIOFeature3D& ft1, const VIOFeature3D& ft2) {
		return (ft1.variance < ft2.variance);
	}
};

void VIO::sortActive3DFeaturesByVariance() {
	std::sort(active3DFeatures.begin(), active3DFeatures.end(),
			less_than_key());
}

// FROM Theia by Chris Sweeney

// Given either a fundamental or essential matrix and two corresponding images
// points such that ematrix * point2 produces a line in the first image,
// this method finds corrected image points such that
// corrected_point1^t * ematrix * corrected_point2 = 0.
void VIO::FindOptimalImagePoints(const Eigen::Matrix3d& ematrix,
		const Eigen::Vector2d& point1, const Eigen::Vector2d& point2,
		Eigen::Vector2d* corrected_point1, Eigen::Vector2d* corrected_point2) {
	const Eigen::Vector3d point1_homog = point1.homogeneous();
	const Eigen::Vector3d point2_homog = point2.homogeneous();

	// A helper matrix to isolate certain coordinates.
	Eigen::Matrix<double, 2, 3> s_matrix;
	s_matrix << 1, 0, 0, 0, 1, 0;

	const Eigen::Matrix2d e_submatrix = ematrix.topLeftCorner<2, 2>();

	// The epipolar line from one image point in the other image.
	Eigen::Vector2d epipolar_line1 = s_matrix * ematrix * point2_homog;
	Eigen::Vector2d epipolar_line2 = s_matrix * ematrix.transpose()
					* point1_homog;

	const double a = epipolar_line1.transpose() * e_submatrix * epipolar_line2;
	const double b = (epipolar_line1.squaredNorm()
			+ epipolar_line2.squaredNorm()) / 2.0;
	const double c = point1_homog.transpose() * ematrix * point2_homog;

	const double d = sqrt(b * b - a * c);

	double lambda = c / (b + d);
	epipolar_line1 -= e_submatrix * lambda * epipolar_line1;
	epipolar_line2 -= e_submatrix.transpose() * lambda * epipolar_line2;

	lambda *= (2.0 * d)
					/ (epipolar_line1.squaredNorm() + epipolar_line2.squaredNorm());

	*corrected_point1 = (point1_homog
			- s_matrix.transpose() * lambda * epipolar_line1).hnormalized();
	*corrected_point2 = (point2_homog
			- s_matrix.transpose() * lambda * epipolar_line2).hnormalized();
}

// Triangulates 2 posed views
bool VIO::TriangulateDLT(const Matrix3x4d& pose1, const Matrix3x4d& pose2,
		const Eigen::Vector2d& point1, const Eigen::Vector2d& point2,
		Eigen::Vector4d* triangulated_point) {
	Eigen::Matrix4d design_matrix;
	design_matrix.row(0) = point1[0] * pose1.row(2) - pose1.row(0);
	design_matrix.row(1) = point1[1] * pose1.row(2) - pose1.row(1);
	design_matrix.row(2) = point2[0] * pose2.row(2) - pose2.row(0);
	design_matrix.row(3) = point2[1] * pose2.row(2) - pose2.row(1);

	// Extract nullspace.
	*triangulated_point =
			design_matrix.jacobiSvd(Eigen::ComputeFullV).matrixV().rightCols<1>();
	return true;
}


// Ported from Hartley and Zisserman:
// http://www.robots.ox.ac.uk/~vgg/hzbook/code/vgg_multiview/vgg_F_from_P.m
void VIO::FundamentalMatrixFromProjectionMatrices(const double pmatrix1[3 * 4],
		const double pmatrix2[3 * 4], double fmatrix[3 * 3]) {
	Eigen::Map<const Eigen::Matrix<double, 3, 4> > projection1(pmatrix1);
	Eigen::Map<const Eigen::Matrix<double, 3, 4> > projection2(pmatrix2);
	Eigen::Map < Eigen::Matrix3d > fundamental_matrix(fmatrix);

	const int index1[3] = { 1, 2, 0 };
	const int index2[3] = { 2, 0, 1 };
	Eigen::Matrix4d temp_mat;
	for (int r = 0; r < 3; r++) {
		temp_mat.row(2) = projection1.row(index1[r]);
		temp_mat.row(3) = projection1.row(index2[r]);
		for (int c = 0; c < 3; c++) {
			temp_mat.row(0) = projection2.row(index1[c]);
			temp_mat.row(1) = projection2.row(index2[c]);
			fundamental_matrix(r, c) = temp_mat.determinant();
		}
	}
}

// Triangulates 2 posed views
bool VIO::Triangulate(const Matrix3x4d& pose1, const Matrix3x4d& pose2,
		const Eigen::Vector2d& point1, const Eigen::Vector2d& point2,
		Eigen::Vector4d* triangulated_point, Eigen::Matrix3d fmatrix) {

	//FundamentalMatrixFromProjectionMatrices(pose1.data(), pose2.data(), fmatrix.data());

	Eigen::Vector2d corrected_point1, corrected_point2;
	FindOptimalImagePoints(fmatrix, point1, point2, &corrected_point1,
			&corrected_point2);

	// Now the two points are guaranteed to intersect. We can use the DLT method
	// since it is easy to construct.
	return TriangulateDLT(pose1, pose2, corrected_point1, corrected_point2,
			triangulated_point);
}


cv::Matx34d tfTransform2RtMatrix(tf::Transform& t)
{
	cv::Matx34d P(t.getBasis()[0][0], t.getBasis()[0][1], t.getBasis()[0][2], t.getOrigin().x(),
					t.getBasis()[1][0], t.getBasis()[1][1], t.getBasis()[1][2], t.getOrigin().y(),
					t.getBasis()[2][0], t.getBasis()[2][1], t.getBasis()[2][2], t.getOrigin().z());
	return P;
}

/*
 * NOTE: the position of the state, the last state, and the keyframe state must be estimated by this point
 *
 * this function cycles through all matched features from key frame 0,
 * transforms their depth into the current frame
 * calculates their depth in the current frame
 * updates their depth
 * if their depth variance is good enough
 * 	convert the point to a 3d features for future motion estimation
 *
 * x is the state of the current frame
 */
void VIO::updateFeatureDepths(VIOState x, double variance)
{
	tf::StampedTransform base2cam;
	try{
		this->ekf.tf_listener.lookupTransform(this->camera_frame, this->CoM_frame, ros::Time(0), base2cam);
	}
	catch(tf::TransformException& e){
		ROS_WARN_STREAM(e.what());
	}

	Frame& cf = currentFrame();
	Frame& lf = lastFrame();
	KeyFrameInfo& kf = this->keyFrames.at(0);
	ROS_ASSERT(kf.nextFeatureID = this->frameBuffer.at(kf.frameBufferIndex).nextFeatureID);

	cv::Matx34d P1(1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0);

	tf::Transform tf_current = this->cameraTransformFromState(x, base2cam);

	// tf_last * P1_last = tf_current => tf_last.inv() * tf_current = P1_last
	tf::Transform last2current = this->cameraTransformFromState(lf.state, base2cam).inverse() * tf_current;

	// tf_current * P2 = tf_kf => tf_kf * tf_current.inv = P2
	tf::Transform P2_temp = tf_current.inverse() * this->cameraTransformFromState(this->frameBuffer.at(kf.frameBufferIndex).state, base2cam);
	cv::Matx34d P2 = tfTransform2RtMatrix(P2_temp); // this is the transform which converts points in the currentframe to points in the keyframe

	//go through each current feature and transform its depth from the last frame
	for(auto& e : cf.features)
	{
		VIOFeature2D& last_ft = (e.isMatched()) ? lf.features.at(e.getMatchedIndex()) : e;
		//last_ft = lf.features.at(e.getMatchedIndex()); // get the last feature which matches this one
		if(!e.isMatched())
			continue;

		ROS_ASSERT(last_ft.getFeatureID() == e.getMatchedID());

		tf::Vector3 transformedPoint = last2current * (last_ft.getFeatureDepth() * tf::Vector3(last_ft.getUndistorted().x, last_ft.getUndistorted().y, 1.0)); // transform the 3d point from the last frame into the current frame

		//extract the depth from the transformed point and set it
		e.setFeatureDepth(transformedPoint.z()); // the depth would be equal to the new z from the transformed point
		//ROS_DEBUG_STREAM(e.getFeatureDepth());
	}

	cv::Matx<double, 6, 4> A;
	cv::vconcat(P1, P2, A); // construct the A in Ax = b

	//TODO - update the depths of all matched points
	//now that the depths are all corrected for the motion of the camera we can triangulate and do a kalman update on the feature
	for(int i = 0; i < kf.currentFrameIndexes.size(); i++)
	{
		cv::Matx61d b;

		cv::Point2f pt2=kf.matchedFeatures.at(i).getUndistorted(), pt1=cf.features.at(kf.currentFrameIndexes.at(i)).getUndistorted(); // get the two points

		b(0) = pt1.x;
		b(1) = pt1.y;
		b(2) = 1.0;
		b(3) = pt2.x;
		b(4) = pt2.y;
		b(5) = 1.0;

		cv::Matx41d X;

		cv::solve(A, b, X, cv::DECOMP_SVD);

		ROS_DEBUG_STREAM("X: " << X(2)/X(3));

	}
}

void VIO::decomposeEssentialMatrix(cv::Matx33f E, cv::Matx34d& Rt)
{
	cv::SVD svd(E);
	cv::Matx33d W(0,-1,0,   //HZ 9.13
			1,0,0,
			0,0,1);
	cv::Matx33d Winv(0,1,0,
			-1,0,0,
			0,0,1);
	cv::Mat_<double> R = svd.u * cv::Mat(W) * svd.vt; //HZ 9.19
	cv::Mat_<double> t = svd.u.col(2); //u3
	Rt = cv::Matx34d(R(0,0),    R(0,1), R(0,2), t(0),
			R(1,0),    R(1,1), R(1,2), t(1),
			R(2,0),    R(2,1), R(2,2), t(2));
}

/*
void VIO::update3DFeatures()
{
>>>>>>> 98dd91d74806469f2705a7968f81ae787fbc7470
	cv::Mat F;
	cv::Matx33f R;
	cv::Matx31f t;
	std::vector<cv::Point2f> pt1, pt2;
	std::vector<VIOFeature2D> ft1, ft2;
	bool pass;
	double error;
	int match_frame_index;

	//TODO update 3d features using all keyframes
	//error = this->computeFundamentalMatrix(F, R, t, pt1, pt2, pass, ft1, ft2, match_frame_index);

	if (pass && error < MAXIMUM_FUNDAMENTAL_ERROR) {

		cv::Matx34f P1, P2;
		//cv::Mat X_;
		cv::Matx<float, 6, 4> A;

		cv::hconcat(cv::Mat::eye(cv::Size(3, 3), CV_32F),
				cv::Mat::zeros(cv::Size(1, 3), CV_32F), P1);
		cv::hconcat(R, t, P2);
		cv::vconcat(P1, P2, A);

		//cv::triangulatePoints(P1, P2, pt1, pt2, X_);
		//cv::Mat_<float> X = cv::Mat_<float>(4, pt1.size());
		//X_.copyTo(X);
		//ROS_ASSERT(ft2.size() == pt2.size() && ft2.size() == X.cols);

		tf::StampedTransform base2cam;
		try {
			this->ekf.tf_listener.lookupTransform(this->camera_frame,
					this->CoM_frame, ros::Time(0), base2cam);
		} catch (tf::TransformException& e) {
			ROS_WARN_STREAM(e.what());
		}

		std::vector<VIOFeature3D> inactives, actives;

		inactives = this->active3DFeatures;

		for (int i = 0; i < ft2.size(); i++) {
			VIOFeature3D matched3dFeature;
			bool matched3d = false;

			for (int j = 0; j < inactives.size(); j++) {
				if (inactives.at(j).current2DFeatureMatchIndex
						== ft2.at(i).getMatchedIndex()) {
					ROS_ASSERT(
							inactives.at(j).current2DFeatureMatchID
							== ft2.at(i).getMatchedID());
					matched3d = true;
					matched3dFeature = inactives.at(j);

					inactives.erase(inactives.begin() + j);

					break;
				}
			}

			cv::Matx41f X;
			cv::Matx61f b;

			b(0) = pt1.at(i).x;
			b(1) = pt1.at(i).y;
			b(2) = 1.0;
			b(3) = pt2.at(i).x;
			b(4) = pt2.at(i).y;
			b(5) = 1.0;

			cv::solve(A, b, X, cv::DECOMP_SVD);

			cv::Matx61f b_ = A * X;
			b_(0) /= b_(2);
			b_(1) /= b_(2);
			b_(2) = 1.0;
			b_(3) /= b_(5);
			b_(4) /= b_(5);
			b_(5) = 1.0;

			double reprojError = cv::norm(b_ - b);

			//CONVERT THE POINT INTO THE WORLD COORDINATE FRAME
			VIOState centerState = this->frameBuffer.at(match_frame_index).state;

			tf::Vector3 r_c = tf::Vector3(X(0) / X(3), X(1) / X(3),
					X(2) / X(3));

			tf::Vector3 r_b = tf::Vector3(centerState.x(), centerState.y(),
					centerState.z());
			tf::Quaternion q_b = tf::Quaternion(centerState.q1(),
					centerState.q2(), centerState.q3(), centerState.q0());
			tf::Transform w2b = tf::Transform(q_b, r_b);

			tf::Transform w2c = w2b * base2cam;

			tf::Vector3 r_w = w2c.inverse() * r_c;

			//ROS_DEBUG_STREAM("point: " << r_c.x() << ", " << r_c.y() << ", " << r_c.z());
			//ROS_DEBUG_STREAM("reproj error: " << reprojError);

			//UPDATE THE 3d POINT OR ADD IT

			if (r_c.z() > MIN_TRIAG_Z && reprojError < MAX_TRIAG_ERROR) {
				if (matched3d) {
					//ROS_DEBUG_STREAM("updating 3d feature");
					matched3dFeature.current2DFeatureMatchID =
							ft2.at(i).getFeatureID();
					matched3dFeature.current2DFeatureMatchIndex = i;
					matched3dFeature.update(
							Eigen::Vector3d(r_w.x(), r_w.y(), r_w.z()),
							reprojError);
					actives.push_back(matched3dFeature);
				} else {
					//ROS_DEBUG_STREAM("adding new 3d feature");
					VIOFeature3D ft3d;
					ft3d.current2DFeatureMatchID = ft2.at(i).getFeatureID();
					ft3d.current2DFeatureMatchIndex = i;
					ft3d.position = Eigen::Vector3d(r_w.x(), r_w.y(), r_w.z());
					ft3d.variance = reprojError;
					actives.push_back(ft3d);
				}
			} else {
				if (matched3d) {
					//ROS_DEBUG_STREAM("bad triag, preserving 3d point without update");
					matched3dFeature.current2DFeatureMatchID =
							ft2.at(i).getFeatureID();
					matched3dFeature.current2DFeatureMatchIndex = i;
					actives.push_back(matched3dFeature);
				}
			}

			if (matched3d) {
				tf::Vector3 pos = w2c
						* tf::Vector3(matched3dFeature.position(0),
								matched3dFeature.position(1),
								matched3dFeature.position(2));
				cv::Matx41f X_;
				X_(0) = pos.x();
				X_(1) = pos.y();
				X_(2) = pos.z();
				X_(3) = 1.0;
				cv::Matx31f b2 = P1 * X_;

				//ROS_DEBUG_STREAM("Projected 3d Point Error: " << tf::Vector3(b2(0)/b2(2) - b(0), b2(1)/b2(2) - b(1), 0).length());
			}

		}
		this->active3DFeatures = actives;
		this->inactive3DFeatures = inactives;
	} else {
		std::vector<VIOFeature3D> inactives, actives;

		inactives = this->active3DFeatures;

		for (int i = 0; i < ft2.size(); i++) {
			VIOFeature3D matched3dFeature;
			bool matched3d = false;

			for (int j = 0; j < inactives.size(); j++) {
				if (inactives.at(j).current2DFeatureMatchIndex
						== ft2.at(i).getMatchedIndex()) {
					ROS_ASSERT(
							inactives.at(j).current2DFeatureMatchID
							== ft2.at(i).getMatchedID());
					matched3d = true;
					matched3dFeature = inactives.at(j);

					inactives.erase(inactives.begin() + j);

					break;
				}
			}

			if (matched3d) {
				//ROS_DEBUG_STREAM("preserving 3d point without update");
				matched3dFeature.current2DFeatureMatchIndex = i;
				matched3dFeature.current2DFeatureMatchID =
						ft2.at(i).getFeatureID();
				actives.push_back(matched3dFeature);
			}
		}

		this->active3DFeatures = actives;
		this->inactive3DFeatures = inactives;
	}
}

 */