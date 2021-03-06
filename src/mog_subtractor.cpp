#include "mog_subtractor.h"


MOGBackgroundSubtraction::MOGBackgroundSubtraction(int _K, int _downsample, float _a, float _T)
{
	nb_gauss = _K;
	downsample = _downsample;
	a = _a;
	T = _T;

	element5 = getStructuringElement(MORPH_RECT, Size(5,5));
	element3 = getStructuringElement(MORPH_RECT, Size(3,3));
}

void MOGBackgroundSubtraction::wrapTransform(Mat& input)
{
	cv::resize(input, input, cv::Size(), 1.0/downsample, 1.0/downsample);
	cv::cvtColor(input, input, CV_BGR2GRAY);
}

void MOGBackgroundSubtraction::init(std::vector<Mat>& imgs)
{
	for(Mat& img : imgs)
    	wrapTransform(img);

    WH = imgs[0].rows*imgs[0].cols;
	std::cout << "Initialization" << std::endl;
	Mat cube;
	zipStdVecToMat(imgs, cube);

	
	Mat _cov = Mat::zeros(WH, nb_gauss, CV_64F);
	Mat _mean = Mat::zeros(WH, nb_gauss, CV_64F); 
	Mat _weight = Mat::zeros(WH, nb_gauss, CV_64F);

	int i;
	auto t1 = std::chrono::high_resolution_clock::now();
	#pragma omp parallel for shared(_cov, _mean, _weight), private(i)
	for(i = 0; i < WH; i++)
	{
		Mat tmp = cv::Mat::zeros(1, cube.cols, CV_8UC1);
		cube.row(i).copyTo(tmp);
		train(tmp, i, _cov, _mean, _weight);
	}
	auto t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> fp_ms = t2 - t1;
    std::cout << WH << " EM algo performed in " << fp_ms.count() << "ms" << std::endl;

    cov = _cov;
    mean = _mean;
    weight = _weight;
}

void MOGBackgroundSubtraction::zipStdVecToMat(std::vector<Mat>& imgs, Mat& output)
{
	Mat cube = Mat::zeros(imgs.size(), WH, CV_8UC1); 

	for(int idx = 0; idx < imgs.size(); idx++)
	{
		Mat tmp(1, WH, CV_8UC1, imgs[idx].data);
		tmp.copyTo(cube.row(idx));
	}
	output = cube.t();
}

void MOGBackgroundSubtraction::train(Mat& line, int id_line, Mat& _cov, Mat& _mean, Mat& _weight)
{	
    Ptr<EM> model = EM::create();
    model->setClustersNumber(nb_gauss);
    model->setCovarianceMatrixType(EM::COV_MAT_DIAGONAL);
    model->setTermCriteria(TermCriteria(TermCriteria::COUNT+TermCriteria::EPS, 100, 0.1));
    model->trainEM( line.t() );

	Mat m = model->getMeans().t();
	std::vector<Mat> c; model->getCovs(c);
	Mat w = model->getWeights();

	m.copyTo(_mean.row(id_line));
	w.copyTo(_weight.row(id_line));
	hconcat(c, _cov.row(id_line));

	// in order to avoid lim cov = 0
	Mat tmp;
	threshold(_cov.row(id_line), tmp, cov_min, cov_min, THRESH_BINARY_INV);
	_cov.row(id_line) += tmp;
}


Mat MOGBackgroundSubtraction::createMask(Mat& img)
{
	wrapTransform(img);

	Mat mask = Mat::zeros(img.rows, img.cols, CV_8UC1);
	Mat maha, mask_own, prob, least_prob;
	divide(weight, cov, least_prob);
	isInGaussian(img, maha, mask_own);
	computeGaussianProbDensity(maha, prob);
	masking(img, mask, mask_own, least_prob, prob);
	morphoOp(mask);

	return mask;
}

void MOGBackgroundSubtraction::isInGaussian(Mat& X, Mat& maha, Mat& mask_own)
{	
	Mat in = Mat(WH,1, X.type(), X.data);
	Mat out;
	repeat(in, 1, nb_gauss, out);

	out.convertTo(out, CV_64F);
	Mat diff = out-mean;

	Mat sqrt_cov;
	sqrt(cov, sqrt_cov);
	divide(diff, sqrt_cov, maha);

	mask_own = (maha < k) & (maha > -k); //if 255, the gaussian is matched

	multiply(maha, maha, maha);
}

void MOGBackgroundSubtraction::computeGaussianProbDensity(Mat& maha, Mat& probDensity)
{
	probDensity = -0.5*maha;
	exp(probDensity, probDensity);
	Mat sqrt_cov;
	sqrt(2*CV_PI*cov, sqrt_cov);
	divide(a*probDensity, sqrt_cov, probDensity);
}

void MOGBackgroundSubtraction::masking(Mat& X, Mat& mask, Mat& mask_own, Mat& least_prob, Mat& prob)
{
	auto t1 = std::chrono::high_resolution_clock::now();
	Mat what_case;
	reduce(mask_own, what_case, 1, CV_REDUCE_SUM, CV_32SC1); 
	what_case = what_case > 0; // if 255, a gaussian is matched

	//For OpenMP
	Mat _cov = cov;
	Mat _mean = mean;
	Mat _weight = weight;

	uchar* case_data = what_case.data;
	uchar* mask_data = mask.data;
	uchar* pixel = X.data;
	int i;

	#pragma omp parallel for shared(least_prob, prob, _cov, _mean, _weight, case_data, mask_data, pixel), private(i)
	for(i = 0; i < WH; i++)
	{
		Mat tmp_p = prob.row(i), tmp_l = least_prob.row(i), tmp_c = _cov.row(i), tmp_m = _mean.row(i), tmp_w = _weight.row(i);
		uchar cas = case_data[i];
		uchar pix = pixel[i];
		if(cas)
		{
			double min, max; int minidx[2], maxidx[2];
			Mat tmp_match = mask_own.row(i);
			minMaxIdx(tmp_w, &min, &max, minidx, maxidx, tmp_match);

			if(tmp_w.at<double>(0,maxidx[1]) > T)
				mask_data[i] = 0;
			else
				mask_data[i] = 255;

			update_case1(pix, maxidx[1], tmp_p, tmp_c, tmp_m, tmp_w);
		}
		else
		{
			mask_data[i] = 255;			
			update_case2(pix, tmp_l, tmp_c, tmp_m, tmp_w);
		}
	}

	/*mask.forEach<uint8_t>
	(
		[&](uint8_t &p, const int position[])->void{
			int i = position[0]*position[1];

			Mat tmp_p = prob.row(i), tmp_l = least_prob.row(i), tmp_c = cov.row(i), tmp_m = mean.row(i), tmp_w = weight.row(i);
			uchar cas = case_data[i];
			uchar pix = pixel[i];
			if(cas)
			{
				double min, max; int minidx[2], maxidx[2];
				Mat tmp_match = mask_own.row(i);
				minMaxIdx(tmp_w, &min, &max, minidx, maxidx, tmp_match);

				if(tmp_w.at<double>(0,maxidx[1]) > T)
					p = 0;
				else
					p = 255;

				update_case1(pix, maxidx[1], tmp_p, tmp_c, tmp_m, tmp_w);
			}
			else
			{
				p = 255;			
				update_case2(pix, tmp_l, tmp_c, tmp_m, tmp_w);
			}
		}
	);*/

	auto t2 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> fp_ms = t2 - t1;
    std::cout << "Frame masked in " << fp_ms.count() << "ms" << std::endl;
}

void MOGBackgroundSubtraction::morphoOp(Mat& mask)
{
	erode(mask,mask, element3);
	dilate(mask, mask, element5);
	dilate(mask, mask, element5);	
}

void MOGBackgroundSubtraction::update_case1(uchar X, int idx_match, Mat& prob, Mat& _cov, Mat& _mean, Mat& _weight)
{
	for(int i = 0; i < nb_gauss; i++)
	{
		if(i == idx_match)
		{
			double r = prob.at<double>(0,i);
			_weight.at<double>(0,i) = (1.0-a)*_weight.at<double>(0,i)+a;
			_mean.at<double>(0,i) = (1.0-r)*_mean.at<double>(0,i) + r*(double)X;

			double m = _mean.at<double>(0,i);
			double val = (1.0-r)*_cov.at<double>(0,i) + r*((double)X - m)*((double)X - m);
			if(val < cov_min)
				_cov.at<double>(0,i) = cov_min;
			else
				_cov.at<double>(0,i) = val;
		}
		else
			_weight.at<double>(0,i) = (1.0-a)*_weight.at<double>(0,i);
	}

}

void MOGBackgroundSubtraction::update_case2(uchar X, Mat& least_prob, Mat& _cov, Mat& _mean, Mat& _weight)
{
	Mat idx;
	sortIdx(least_prob, idx, CV_SORT_EVERY_ROW + CV_SORT_ASCENDING);
	int least = idx.data[0];

	_cov.at<double>(0,least) = 100;
	_mean.at<double>(0, least) = (double)X;
	_weight.at<double>(0, least) = 0.1;
}