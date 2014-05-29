/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    Michael R. Smith,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or find a way to pay it forward. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/

#include "GRecommender.h"
#include "GRecommenderLib.h"
#include "GSparseMatrix.h"
#include "GCluster.h"
#include "GMatrix.h"
#include "GActivation.h"
#include "GHeap.h"
#include "GRand.h"
#include "GNeuralNet.h"
#include "GDistance.h"
#include "GVec.h"
#include <math.h>
#include <map>
#include <vector>
#include <cmath>
#include "GDom.h"
#include "GTime.h"
#include "GHolders.h"
#include "GApp.h"
#include "GLearner.h"
#include "GLearnerLib.h"
#include "usage.h"

using std::map;
using std::multimap;
using std::pair;
using std::set;
using std::vector;

namespace GClasses {

void GCollaborativeFilter_dims(GMatrix& data, size_t* pOutUsers, size_t* pOutItems)
{
	double m = data.columnMin(0);
	double r = data.columnMax(0);
	if(m < 0)
		throw Ex("col 0 (user) indexes out of range");
	*pOutUsers = size_t(ceil(r)) + 1;
	m = data.columnMin(1);
	r = data.columnMax(1);
	if(m < 0)
		throw Ex("col 1 (item) indexes out of range");
	*pOutItems = size_t(ceil(r)) + 1;
	if(data.rows() * 8 < *pOutUsers)
		throw Ex("col 0 (user) indexes out of range");
	if(data.rows() * 8 < *pOutItems)
		throw Ex("col 1 (item) indexes out of range");
}

GCollaborativeFilter::GCollaborativeFilter()
: m_rand(0)
{
}

GCollaborativeFilter::GCollaborativeFilter(GDomNode* pNode, GLearnerLoader& ll)
: m_rand(0)
{
}

void GCollaborativeFilter::trainDenseMatrix(const GMatrix& data, const GMatrix* pLabels)
{
	if(!data.relation().areContinuous())
		throw Ex("GCollaborativeFilter::trainDenseMatrix only supports continuous attributes.");

	// Convert to 3-column form
	GMatrix* pMatrix = new GMatrix(0, 3);
	Holder<GMatrix> hMatrix(pMatrix);
	size_t dims = data.cols();
	for(size_t i = 0; i < data.rows(); i++)
	{
		const double* pRow = data.row(i);
		for(size_t j = 0; j < dims; j++)
		{
			if(*pRow != UNKNOWN_REAL_VALUE)
			{
				double* pVec = pMatrix->newRow();
				pVec[0] = i;
				pVec[1] = j;
				pVec[2] = *pRow;
			}
			pRow++;
		}
	}

	if(pLabels)
	{
		size_t labelDims = pLabels->cols();
		for(size_t i = 0; i < pLabels->rows(); i++)
		{
			const double* pRow = pLabels->row(i);
			for(size_t j = 0; j < labelDims; j++)
			{
				if(*pRow != UNKNOWN_REAL_VALUE)
				{
					double* pVec = pMatrix->newRow();
					pVec[0] = i;
					pVec[1] = dims + j;
					pVec[2] = *pRow;
				}
				pRow++;
			}
		}
	}

	// Train
	train(*pMatrix);
}

GDomNode* GCollaborativeFilter::baseDomNode(GDom* pDoc, const char* szClassName) const
{
	GDomNode* pNode = pDoc->newObj();
	pNode->addField(pDoc, "class", pDoc->newString(szClassName));
	return pNode;
}

double GCollaborativeFilter::crossValidate(GMatrix& data, size_t folds, double* pOutMAE)
{
	// Randomly assign each rating to one of the folds
	size_t ratings = data.rows();
	size_t* pFolds = new size_t[ratings];
	ArrayHolder<size_t> hFolds(pFolds);
	for(size_t i = 0; i < ratings; i++)
		pFolds[i] = (size_t)m_rand.next(folds);

	// Evaluate accuracy
	double ssse = 0.0;
	double smae = 0.0;
	for(size_t i = 0; i < folds; i++)
	{
		// Split the data
		GMatrix dataTrain(data.relation().clone());
		GReleaseDataHolder hDataTrain(&dataTrain);
		GMatrix dataTest(data.relation().clone());
		GReleaseDataHolder hDataTest(&dataTest);
		size_t* pF = pFolds;
		for(size_t j = 0; j < data.rows(); j++)
		{
			if(*pF == i)
				dataTest.takeRow(data[j]);
			else
				dataTrain.takeRow(data[j]);
			pF++;
		}

		double mae;
		ssse += trainAndTest(dataTrain, dataTest, &mae);
		smae += mae;
	}

	if(pOutMAE)
		*pOutMAE = smae / folds;
	return ssse / folds;
}

double GCollaborativeFilter::trainAndTest(GMatrix& dataTrain, GMatrix& dataTest, double* pOutMAE)
{
	train(dataTrain);
	double sse = 0.0;
	double se = 0.0;
	size_t hits = 0;
	for(size_t j = 0; j < dataTest.rows(); j++)
	{
		double* pVec = dataTest[j];
		double prediction = predict(size_t(pVec[0]), size_t(pVec[1]));
		if (prediction < -1e100 || prediction > 1e100)
		{
			throw Ex("Unreasonable prediction");
		}
		double err = pVec[2] - prediction;
		se += std::abs(err);
		sse += (err * err);
		hits++;
	}
	if(pOutMAE)
		*pOutMAE = se / dataTest.rows();
	return sse / dataTest.rows();
}

class TarPredComparator
{
public:
	TarPredComparator() {}

	bool operator() (const std::pair<double,double>& a, const std::pair<double,double>& b) const
	{
		return a.second > b.second;
	}
};

GMatrix* GCollaborativeFilter::precisionRecall(GMatrix& data, bool ideal)
{
	// Divide into two equal-size folds
	size_t ratings = data.rows();
	size_t halfRatings = ratings / 2;
	size_t* pFolds = new size_t[ratings];
	size_t f0 = ratings - halfRatings;
	size_t f1 = halfRatings;
	for(size_t i = 0; i < ratings; i++)
	{
		if(m_rand.next(f0 + f1) < f0)
		{
			pFolds[i] = 0;
			f0--;
		}
		else
		{
			pFolds[i] = 1;
			f1--;
		}
	}

	// Make a vector of target values and corresponding predictions
	vector<std::pair<double,double> > tarPred;
	tarPred.reserve(halfRatings);

	// Split the data
	GMatrix dataTrain(data.relation().clone());
	GReleaseDataHolder hDataTrain(&dataTrain);
	GMatrix dataTest(data.relation().clone());
	GReleaseDataHolder hDataTest(&dataTest);
	size_t* pF = pFolds;
	for(size_t j = 0; j < data.rows(); j++)
	{
		if(*pF == 0)
			dataTrain.takeRow(data[j]);
		else
			dataTest.takeRow(data[j]);
		pF++;
	}

	if(ideal)
	{
		// Simulate perfect predictions
		for(size_t i = 0; i < dataTest.rows(); i++)
		{
			double* pVec = dataTest[i];
			tarPred.push_back(std::make_pair(pVec[2], pVec[2]));
		}
	}
	else
	{
		// Train
		train(dataTrain);

		// Predict the ratings in the test data
		for(size_t i = 0; i < dataTest.rows(); i++)
		{
			double* pVec = dataTest[i];
			double prediction = predict(size_t(pVec[0]), size_t(pVec[1]));
			GAssert(prediction != UNKNOWN_REAL_VALUE);
			tarPred.push_back(std::make_pair(pVec[2], prediction));
		}
	}

	// Make precision-recall data
	TarPredComparator comp;
	std::sort(tarPred.begin(), tarPred.end(), comp);
	double totalRelevant = 0.0;
	double totalIrrelevant = 0.0;
	for(vector<std::pair<double,double> >::iterator it = tarPred.begin(); it != tarPred.end(); it++)
	{
		totalRelevant += it->first;
		totalIrrelevant += (1.0 - it->first); // Here we assume that all ratings range from 0 to 1.
	}
	double retrievedRelevant = 0.0;
	double retrievedIrrelevant = 0.0;
	GMatrix* pResults = new GMatrix(0, 3);
	for(vector<std::pair<double,double> >::iterator it = tarPred.begin(); it != tarPred.end(); it++)
	{
		retrievedRelevant += it->first;
		retrievedIrrelevant += (1.0 - it->first); // Here we assume that all ratings range from 0 to 1.
		double precision = retrievedRelevant / (retrievedRelevant + retrievedIrrelevant);
		double recall = retrievedRelevant / totalRelevant; // recall is the same as the truePositiveRate
		double falsePositiveRate = retrievedIrrelevant / totalIrrelevant;
		double* pRow = pResults->newRow();
		pRow[0] = recall;
		pRow[1] = precision;
		pRow[2] = falsePositiveRate;
	}
	return pResults;
}

// static
double GCollaborativeFilter::areaUnderCurve(GMatrix& data)
{
	double a = 0.0;
	double b = 0.0;
	double prevX = 0.0;
	double prevY = 0.0;
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pRow = data[i];
		a += (pRow[2] - prevX) * pRow[0];
		b += (pRow[2] - prevX) * prevY;
		prevX = pRow[2];
		prevY = pRow[0];
	}
	a += 1.0 - prevX;
	b += (1.0 - prevX) * prevY;
	return 0.5 * (a + b);
}

#ifndef NO_TEST_CODE
void GCF_basicTest_makeData(GMatrix& m, GRand& rand)
{
	// Generate perfectly linear ratings based on random preferences
	// with both item and user bias
	for(size_t i = 0; i < 300; i++)
	{
		double a = rand.uniform();
		double b = rand.normal();
		double c = rand.uniform();
		double userBias = rand.normal();
		double* pVec;
		pVec = m.newRow();
		pVec[0] = i; // user
		pVec[1] = 0; // item
		pVec[2] = a + 0.0 + 0.2 * c + userBias; // rating
		pVec = m.newRow();
		pVec[0] = i; // user
		pVec[1] = 1; // item
		pVec[2] = 0.2 * a + 0.2 * b + c * c + 0.2 + userBias; // rating
		pVec = m.newRow();
		pVec[0] = i; // user
		pVec[1] = 2; // item
		pVec[2] = 0.6 * a + 0.1 * b + 0.2 * c * c * c - 0.3 + userBias; // rating
		pVec = m.newRow();
		pVec[0] = i; // user
		pVec[1] = 3; // item
		pVec[2] = 0.5 * a + 0.5 * b - 0.5 * c + 0.0 + userBias; // rating
		pVec = m.newRow();
		pVec[0] = i; // user
		pVec[1] = 4; // item
		pVec[2] = -0.2 * a + 0.4 * b - 0.3 * sin(c) + 0.1 + userBias; // rating
	}
}

void GCollaborativeFilter::basicTest(double maxMSE)
{
	GRand rand(0);
	GMatrix m(0, 3);
	GCF_basicTest_makeData(m, rand);
	double mse = crossValidate(m, 2);
	if(mse > maxMSE)
		throw Ex("Failed. Expected MSE=", to_str(maxMSE), ". Actual MSE=", to_str(mse), ".");
	else if(mse + 0.085 < maxMSE)
		std::cerr << "\nTest needs to be tightened. MSE: " << mse << ", maxMSE: " << maxMSE << "\n";
}
#endif





GBaselineRecommender::GBaselineRecommender()
: GCollaborativeFilter(), m_pRatings(NULL), m_items(0)
{
}

GBaselineRecommender::GBaselineRecommender(GDomNode* pNode, GLearnerLoader& ll)
: GCollaborativeFilter(pNode, ll)
{
	GDomListIterator it(pNode->field("ratings"));
	m_items = it.remaining();
	m_pRatings = new double[m_items];
	GVec::deserialize(m_pRatings, it);
}

// virtual
GBaselineRecommender::~GBaselineRecommender()
{
	delete[] m_pRatings;
}

// virtual
void GBaselineRecommender::train(GMatrix& data)
{
	// Determine the sizes
	if(data.cols() != 3)
		throw Ex("Expected 3 cols");
//	double m = data.columnMin(1);
	double r = data.columnMax(1);
	m_items = size_t(ceil(r)) + 1;
	if(data.rows() * 8 < m_items)
		throw Ex("column 1 (item) indexes out of range");

	// Allocate space
	delete[] m_pRatings;
	m_pRatings = new double[m_items];
	size_t* pCounts = new size_t[m_items];
	ArrayHolder<size_t> hCounts(pCounts);
	size_t* pC = pCounts;
	double* pR = m_pRatings;
	for(size_t i = 0; i < m_items; i++)
	{
		*pC = 0;
		pC++;
		*pR = 0.0;
		pR++;
	}
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data[i];
		pVec++;
		size_t c = size_t(*pVec);
		pVec++;
		pR = m_pRatings + c;
		*pR *= ((double)pCounts[c] / (pCounts[c] + 1));
		*pR += (*pVec / (pCounts[c] + 1));
		pCounts[c]++;
	}
}

// virtual
double GBaselineRecommender::predict(size_t user, size_t item)
{
	if(item >= m_items)
		return 0.0;
	return m_pRatings[item];
}

// virtual
void GBaselineRecommender::impute(double* pVec, size_t dims)
{
	size_t n = std::min(dims, m_items);
	size_t i;
	for(i = 0; i < n; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			*pVec = m_pRatings[i];
		pVec++;
	}
	for( ; i < dims; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			*pVec = 0.0;
	}
}

// virtual
GDomNode* GBaselineRecommender::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GBaselineRecommender");
	pNode->addField(pDoc, "ratings", GVec::serialize(pDoc, m_pRatings, m_items));
	return pNode;
}

#ifndef NO_TEST_CODE
// static
void GBaselineRecommender::test()
{
	GBaselineRecommender rec;
	rec.basicTest(1.16);
}
#endif







GInstanceRecommender::GInstanceRecommender(size_t neighbors)
: GCollaborativeFilter(), m_neighbors(neighbors), m_ownMetric(true), m_pData(NULL), m_pBaseline(NULL), m_significanceWeight(0)
{
	m_pMetric = new GCosineSimilarity();
}

GInstanceRecommender::GInstanceRecommender(GDomNode* pNode, GLearnerLoader& ll)
: GCollaborativeFilter(pNode, ll)
{
	m_neighbors = (size_t)pNode->field("neighbors")->asInt();
	m_pMetric = GSparseSimilarity::deserialize(pNode->field("metric"));
	m_ownMetric = true;
	m_pData = new GSparseMatrix(pNode->field("data"));
	m_pBaseline = new GBaselineRecommender(pNode->field("bl"), ll);
	m_significanceWeight = (size_t)pNode->field("sigWeight")->asInt();
}

// virtual
GInstanceRecommender::~GInstanceRecommender()
{
	delete(m_pData);
	if(m_ownMetric)
		delete(m_pMetric);
	delete(m_pBaseline);
}

void GInstanceRecommender::setMetric(GSparseSimilarity* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

// virtual
void GInstanceRecommender::train(GMatrix& data)
{
	if(data.cols() != 3)
		throw Ex("Expected 3 cols");

	// Compute the baseline recommendations
	delete(m_pBaseline);
	m_pBaseline = new GBaselineRecommender();
	m_pBaseline->train(data);

	// Store the data
	size_t users, items;
	GCollaborativeFilter_dims(data, &users, &items);
	delete(m_pData);
	m_pData = new GSparseMatrix(users, items, UNKNOWN_REAL_VALUE);
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data[i];
		m_pData->set(size_t(pVec[0]), size_t(pVec[1]), pVec[2]);
	}
}

// virtual
double GInstanceRecommender::predict(size_t user, size_t item)
{
        if(!m_pData)
                throw Ex("This model has not been trained");
        if(user >= m_pData->rows() || item >= m_pData->cols())
                return 0.0;

        // Find the k-nearest neighbors
        multimap<double,size_t> depq; // double-ended priority-queue that maps from similarity to user-id
        for(size_t neigh = 0; neigh < m_pData->rows(); neigh++)
        {
                // Only consider other users that have rated this item
                if(neigh == user)
                        continue;
                double rating = m_pData->get(neigh, item);
                if(rating == UNKNOWN_REAL_VALUE)
                        continue;

                // Compute the similarity
                size_t count = 0;
                double similarity = m_pMetric->similarity(m_pData->row(user), m_pData->row(neigh), count);

                if(count < m_significanceWeight)
                        similarity *= count / m_significanceWeight;

                // If the queue is overfull, drop the worst item
                depq.insert(std::make_pair(similarity, neigh));
                if(depq.size() > m_neighbors)
                        depq.erase(depq.begin());
        }

        // Combine the ratings of the nearest neighbors to make a prediction
        double weighted_sum = 0.0;
        double sum_weight = 0.0;
        for(multimap<double,size_t>::iterator it = depq.begin(); it != depq.end(); it++)
        {
                double weight = std::max(0.0, std::min(1.0, it->first));
                double val = m_pData->get(it->second, item);
                weighted_sum += weight * val;
                sum_weight += weight;
        }
        if(sum_weight > 0.0)
                return weighted_sum / sum_weight;
        else
                return m_pBaseline->predict(user, item);
}

multimap<double,ArrayWrapper> GInstanceRecommender::getNeighbors(size_t user, size_t item)
{
	if(!m_pData)
		throw Ex("This model has not been trained");
	if(user >= m_pData->rows() || item >= m_pData->cols())
		throw Ex("User and/or item not in the provided data set");

	// Find the k-nearest neighbors
        if(m_user_depq.find(user) == m_user_depq.end())
        {
		multimap<double,ArrayWrapper> depq; // double-ended priority-queue that maps from similarity to user-id
		for(size_t neigh = 0; neigh < m_pData->rows(); neigh++)
		{
			// Only consider other users that have rated this item
			if(neigh == user)
				continue;
			double rating = m_pData->get(neigh, item);
			if(rating == UNKNOWN_REAL_VALUE)
				continue;

			// Compute the similarity
			size_t count = 0;
			double similarity = m_pMetric->similarity(m_pData->row(user), m_pData->row(neigh), count);

			if(count < m_significanceWeight)
				similarity *= count / m_significanceWeight;

			// If the queue is overfull, drop the worst item
			ArrayWrapper temp = {{neigh, count}};
			depq.insert(std::make_pair(similarity, temp));
			if(depq.size() > m_neighbors)
				depq.erase(depq.begin());
		}
                m_user_depq[user] = depq;
        }

	return m_user_depq[user];
}

// virtual
void GInstanceRecommender::impute(double* pVec, size_t dims)
{
	if(!m_pData)
		throw Ex("This model has not been trained");
	if(dims != m_pData->cols())
		throw Ex("The vector has a different size than this model was trained with");

	// Find the k-nearest neighbors
	multimap<double,size_t> depq; // double-ended priority-queue that maps from similarity to user-id
	for(size_t neigh = 0; neigh < m_pData->rows(); neigh++)
	{
		// Compute the similarity
		size_t count = 0;
		double similarity = m_pMetric->similarity(m_pData->row(neigh), pVec, count);

		if(count < m_significanceWeight)
			similarity *= count / m_significanceWeight;

		// If the queue is overfull, drop the worst item
		depq.insert(std::make_pair(similarity, neigh));
		if(depq.size() > m_neighbors)
			depq.erase(depq.begin());
	}

	// Impute missing values by combining the ratings from the neighbors
	for(size_t i = 0; i < m_pData->cols(); i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
		{
			double weighted_sum = 0.0;
			double sum_weight = 0.0;
			for(multimap<double,size_t>::iterator it = depq.begin(); it != depq.end(); it++)
			{
				double val = m_pData->get(it->second, i);
				if(val != UNKNOWN_REAL_VALUE)
				{
					double weight = std::max(0.0, std::min(1.0, it->first));
					weighted_sum += weight * val;
					sum_weight += weight;
				}
			}
			if(sum_weight > 0.0)
				pVec[i] = weighted_sum / sum_weight;
			else
				pVec[i] = m_pBaseline->predict(0, i); // baseline ignores the user
		}
	}
}

// virtual
GDomNode* GInstanceRecommender::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GInstanceRecommender");
	pNode->addField(pDoc, "neighbors", pDoc->newInt(m_neighbors));
	pNode->addField(pDoc, "metric", m_pMetric->serialize(pDoc));
	pNode->addField(pDoc, "data", m_pData->serialize(pDoc));
	pNode->addField(pDoc, "bl", m_pBaseline->serialize(pDoc));
	pNode->addField(pDoc, "sigWeight", pDoc->newInt(m_significanceWeight));
	return pNode;
}

double GInstanceRecommender::getRating(size_t user, size_t item)
{
	return m_pData->get(user, item);
}

#ifndef NO_TEST_CODE
// static
void GInstanceRecommender::test()
{
	GInstanceRecommender rec(8);
	rec.basicTest(0.63);
}
#endif





GSparseClusterRecommender::GSparseClusterRecommender(size_t clusters)
: GCollaborativeFilter(), m_clusters(clusters), m_pPredictions(NULL), m_pClusterer(NULL), m_ownClusterer(false), m_users(0), m_items(0)
{
}

// virtual
GSparseClusterRecommender::~GSparseClusterRecommender()
{
	if(m_ownClusterer)
		delete(m_pClusterer);
	delete(m_pPredictions);
}

void GSparseClusterRecommender::setClusterer(GSparseClusterer* pClusterer, bool own)
{
	if(pClusterer->clusterCount() != m_clusters)
		throw Ex("Mismatching number of clusters");
	if(m_ownClusterer)
		delete(m_pClusterer);
	m_pClusterer = pClusterer;
	m_ownClusterer = own;
}

// virtual
void GSparseClusterRecommender::train(GMatrix& data)
{
	if(data.cols() != 3)
		throw Ex("Expected 3 cols");

	// Convert the data to a sparse matrix
	size_t users, items;
	GCollaborativeFilter_dims(data, &users, &items);
	m_users = users;
	m_items = items;
	GSparseMatrix sm(users, items, UNKNOWN_REAL_VALUE);
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data.row(i);
		sm.set(size_t(pVec[0]), size_t(pVec[1]), pVec[2]);
	}

	// Make sure we have a clusterer
	if(!m_pClusterer)
		setClusterer(new GKMeansSparse(m_clusters, &m_rand), true);

	// Cluster the data
	m_pClusterer->cluster(&sm);

	// Gather the mean predictions in each cluster
	delete(m_pPredictions);
	m_pPredictions = new GMatrix(m_clusters, sm.cols());
	m_pPredictions->setAll(0.0);
	size_t* pCounts = new size_t[sm.cols() * m_clusters];
	ArrayHolder<size_t> hCounts(pCounts);
	memset(pCounts, '\0', sizeof(size_t) * sm.cols() * m_clusters);
	for(size_t i = 0; i < sm.rows(); i++)
	{
		size_t clust = m_pClusterer->whichCluster(i);
		double* pRow = m_pPredictions->row(clust);
		size_t* pRowCounts = pCounts + (sm.cols() * clust);
		for(GSparseMatrix::Iter it = sm.rowBegin(i); it != sm.rowEnd(i); it++)
		{
			pRow[it->first] *= ((double)pRowCounts[it->first] / (pRowCounts[it->first] + 1));
			pRow[it->first] += (it->second / (pRowCounts[it->first] + 1));
			pRowCounts[it->first]++;
		}
	}
}

// virtual
double GSparseClusterRecommender::predict(size_t user, size_t item)
{
	size_t clust = m_pClusterer->whichCluster(user);
	double* pRow = m_pPredictions->row(clust);
	return pRow[item];
}

// virtual
void GSparseClusterRecommender::impute(double* pVec, size_t dims)
{
	throw Ex("Sorry, GSparseClusterRecommender::impute is not yet implemented");
	// todo: Find the closest centroid, and use it to impute all values
}

// virtual
GDomNode* GSparseClusterRecommender::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GSparseClusterRecommender");
	throw Ex("Sorry, this method has not been implemented yet");
	return pNode;
}

#ifndef NO_TEST_CODE
// static
void GSparseClusterRecommender::test()
{
	GSparseClusterRecommender rec(6);
	rec.basicTest(1.31);
}
#endif














GDenseClusterRecommender::GDenseClusterRecommender(size_t clusters)
: GCollaborativeFilter(), m_clusters(clusters), m_pPredictions(NULL), m_pClusterer(NULL), m_ownClusterer(false), m_users(0), m_items(0)
{
}

// virtual
GDenseClusterRecommender::~GDenseClusterRecommender()
{
	if(m_ownClusterer)
		delete(m_pClusterer);
	delete(m_pPredictions);
}

void GDenseClusterRecommender::setClusterer(GClusterer* pClusterer, bool own)
{
	if(pClusterer->clusterCount() != m_clusters)
		throw Ex("Mismatching number of clusters");
	if(m_ownClusterer)
		delete(m_pClusterer);
	m_pClusterer = pClusterer;
	m_ownClusterer = own;
}

void GDenseClusterRecommender::setFuzzifier(double d)
{
	if(!m_pClusterer)
		setClusterer(new GFuzzyKMeans(m_clusters, &m_rand), true);
	((GFuzzyKMeans*)m_pClusterer)->setFuzzifier(d);
}

// virtual
void GDenseClusterRecommender::train(GMatrix& data)
{
	if(data.cols() != 3)
		throw Ex("Expected 3 cols");

	if(!m_pClusterer)
		setClusterer(new GFuzzyKMeans(m_clusters, &m_rand), true);

	// Cluster the data
	size_t users, items;
	GCollaborativeFilter_dims(data, &users, &items);
	m_users = users;
	m_items = items;
	{
		GMatrix dense(users, items);
		for(size_t i = 0; i < data.rows(); i++)
		{
			double* pVec = data.row(i);
			dense[size_t(pVec[0])][size_t(pVec[1])] = pVec[2];
		}
		m_pClusterer->cluster(&dense);
	}

	// Gather the mean predictions in each cluster
	delete(m_pPredictions);
	m_pPredictions = new GMatrix(m_clusters, items);
	m_pPredictions->setAll(0.0);
	size_t* pCounts = new size_t[items * m_clusters];
	ArrayHolder<size_t> hCounts(pCounts);
	memset(pCounts, '\0', sizeof(size_t) * items * m_clusters);
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data.row(i);
		size_t user = size_t(*pVec);
		pVec++;
		size_t item = size_t(*pVec);
		pVec++;
		size_t clust = m_pClusterer->whichCluster(user);
		double* pRow = m_pPredictions->row(clust);
		size_t* pRowCounts = pCounts + (items * clust);
		pRow[item] *= ((double)pRowCounts[item] / (pRowCounts[item] + 1));
		pRow[item] += (*pVec / (pRowCounts[item] + 1));
		pRowCounts[item]++;
	}
}

// virtual
double GDenseClusterRecommender::predict(size_t user, size_t item)
{
	if(user >= m_users || item >= m_items)
		return 0.0;
	size_t clust = m_pClusterer->whichCluster(user);
	double* pRow = m_pPredictions->row(clust);
	return pRow[item];
}

// virtual
void GDenseClusterRecommender::impute(double* pVec, size_t dims)
{
	throw Ex("Sorry, GDenseClusterRecommender::impute is not yet implemented");
	// todo: Find the closest centroid, and use it to impute all values
}

// virtual
GDomNode* GDenseClusterRecommender::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GDenseClusterRecommender");
	throw Ex("Sorry, this method has not been implemented yet");
	return pNode;
}

#ifndef NO_TEST_CODE
// static
void GDenseClusterRecommender::test()
{
	GDenseClusterRecommender rec(6);
	rec.basicTest(0.0);
}
#endif









GMatrixFactorization::GMatrixFactorization(size_t intrinsicDims)
: GCollaborativeFilter(), m_intrinsicDims(intrinsicDims), m_regularizer(0.01), m_pP(NULL), m_pQ(NULL), m_useInputBias(true), m_minIters(1), m_decayRate(0.97)
{
}

GMatrixFactorization::GMatrixFactorization(GDomNode* pNode, GLearnerLoader& ll)
: GCollaborativeFilter(pNode, ll)
{
	m_regularizer = pNode->field("reg")->asDouble();
	m_useInputBias = pNode->field("uib")->asBool();
	m_pP = new GMatrix(pNode->field("p"));
	m_pQ = new GMatrix(pNode->field("q"));
	if(m_pP->cols() != m_pQ->cols())
		throw Ex("Mismatching matrix sizes");
	m_intrinsicDims = m_pP->cols() - 1;
}

// virtual
GMatrixFactorization::~GMatrixFactorization()
{
	delete(m_pQ);
	delete(m_pP);
}

// virtual
GDomNode* GMatrixFactorization::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GMatrixFactorization");
	pNode->addField(pDoc, "reg", pDoc->newDouble(m_regularizer));
	pNode->addField(pDoc, "uib", pDoc->newBool(m_useInputBias));
	pNode->addField(pDoc, "p", m_pP->serialize(pDoc));
	pNode->addField(pDoc, "q", m_pQ->serialize(pDoc));
	return pNode;
}

double GMatrixFactorization::validate(GMatrix& data)
{
	double sse = 0;
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data[i];
		double* pPref = m_pP->row(size_t(pVec[0]));
		double* pWeights = m_pQ->row(size_t(pVec[1]));
		double pred = *(pWeights++) + *(pPref++);
		for(size_t i = 0; i < m_intrinsicDims; i++)
			pred += *(pPref++) * (*pWeights++);
		double err = pVec[2] - pred;
		sse += (err * err);
	}
	return sse;
}

// virtual
void GMatrixFactorization::train(GMatrix& data)
{
	size_t users, items;
	GCollaborativeFilter_dims(data, &users, &items);

	// Initialize P with small random values, and Q with zeros
	delete(m_pP);
	size_t colsP = (m_useInputBias ? 1 : 0) + m_intrinsicDims;
	m_pP = new GMatrix(users, colsP);
	for(size_t i = 0; i < m_pP->rows(); i++)
	{
		double* pVec = m_pP->row(i);
		for(size_t j = 0; j < colsP; j++)
			*(pVec++) = 0.02 * m_rand.normal();
	}
	delete(m_pQ);
	m_pQ = new GMatrix(items, 1 + m_intrinsicDims);
	for(size_t i = 0; i < m_pQ->rows(); i++)
	{
		double* pVec = m_pQ->row(i);
		for(size_t j = 0; j <= m_intrinsicDims; j++)
			*(pVec++) = 0.02 * m_rand.normal();
	}

	// Make a shallow copy of the data (so we can shuffle it)
	GMatrix dataCopy(data.relation().clone());
	GReleaseDataHolder hDataCopy(&dataCopy);
	for(size_t i = 0; i < data.rows(); i++)
		dataCopy.takeRow(data[i]);

	// Train
	double prevErr = 1e308;
	double learningRate = 0.01;
	GTEMPBUF(double, temp_weights, m_intrinsicDims);
	size_t epochs = 0;
	while(learningRate >= 0.001)
	{
		for(size_t iter = 0; iter < m_minIters; iter++)
		{
			// Shuffle the ratings
			dataCopy.shuffle(m_rand);

			// Do an epoch of training
			for(size_t j = 0; j < dataCopy.rows(); j++)
			{
				// Compute the error for this rating
				double* pVec = dataCopy[j];
				double* pPref = m_pP->row(size_t(pVec[0]));
				double* pWeights = m_pQ->row(size_t(pVec[1]));
				double pred = *(pWeights++);
				if(m_useInputBias)
					pred += *(pPref++);
				for(size_t i = 0; i < m_intrinsicDims; i++)
					pred += *(pPref++) * (*pWeights++);
				double err = pVec[2] - pred;

				// Update Q
				pPref = m_pP->row(size_t(pVec[0])) + (m_useInputBias ? 1 : 0);
				double* pT = temp_weights;
				pWeights = m_pQ->row(size_t(pVec[1]));
				*pWeights += learningRate * (err - m_regularizer * (*pWeights));
				pWeights++;
				for(size_t i = 0; i < m_intrinsicDims; i++)
				{
					*(pT++) = *pWeights;
					*pWeights += learningRate * (err * (*pPref) - m_regularizer * (*pWeights));
					pPref++;
					pWeights++;
				}

				// Update P
				pWeights = temp_weights;
				double* pPrefRow = m_pP->row(size_t(pVec[0]));
				pPref = pPrefRow;
				if(m_useInputBias)
				{
					*pPref += learningRate * (err - m_regularizer * (*pPref));
					pPref++;
				}
				for(size_t i = 0; i < m_intrinsicDims; i++)
				{
					*pPref += learningRate * (err * (*pWeights) - m_regularizer * (*pPref));
					pWeights++;
					pPref++;
				}
			}
			epochs++;
		}

		// Stopping criteria
		double rsse = sqrt(validate(data));
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.001) // If the amount of improvement is small
			learningRate *= m_decayRate; // decay the learning rate
		prevErr = rsse;
	}
}

// virtual
double GMatrixFactorization::predict(size_t user, size_t item)
{
	if(!m_pP)
		throw Ex("Not trained yet");
	if(user >= m_pP->rows() || item >= m_pQ->rows())
		return 0.0;
	double* pWeights = m_pQ->row(item);
	double* pPref = m_pP->row(user);
	double pred = *(pWeights++);
	if(m_useInputBias)
		pred += *(pPref++);
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pred += *(pPref++) * (*pWeights++);
	return pred;
}

void GMatrixFactorization_vectorToRatings(const double* pVec, size_t dims, GMatrix& data)
{
	for(size_t i = 0; i < dims; i++)
	{
		if(*pVec != UNKNOWN_REAL_VALUE)
		{
			double* pRow = data.newRow();
			*pRow = 0.0;
			pRow++;
			*pRow = i;
			pRow++;
			*pRow = *pVec;
		}
		pVec++;
	}
}

// virtual
void GMatrixFactorization::impute(double* pVec, size_t dims)
{
	if(!m_pP)
		throw Ex("Not trained yet");

	// Convert the vector to a set of ratings
	GMatrix data(0, 3);
	GMatrixFactorization_vectorToRatings(pVec, std::min(dims, m_pQ->rows()), data);

	// Initialize a preference vector
	GTEMPBUF(double, pPrefVec, (m_useInputBias ? 1 : 0) + m_intrinsicDims);
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pPrefVec[i] = 0.02 * m_rand.normal();

	// Refine the preference vector
	double prevErr = 1e308;
	double learningRate = 0.05;
	while(learningRate >= 0.001)
	{
		// Shuffle the ratings
		data.shuffle(m_rand);

		// Do an epoch of training
		for(size_t i = 0; i < data.rows(); i++)
		{
			// Compute the error for this rating
			double* pVec = data[i];
			double* pPref = pPrefVec;
			double* pWeights = m_pQ->row(size_t(pVec[1]));
			double pred = *(pWeights++);
			if(m_useInputBias)
				pred += *(pPref++);
			for(size_t i = 0; i < m_intrinsicDims; i++)
				pred += *(pPref++) * (*pWeights++);
			double err = pVec[2] - pred;

			// Update the preference vec
			pWeights = m_pQ->row(size_t(pVec[1])) + 1;
			pPref = pPrefVec;
			if(m_useInputBias)
			{
				*pPref += learningRate * err; // regularization is intentionally not used here
				pPref++;
			}
			for(size_t i = 0; i < m_intrinsicDims; i++)
			{
				*pPref += learningRate * err * (*pWeights); // regularization is intentionally not used here
				pWeights++;
				pPref++;
			}
			GVec::floorValues(pPrefVec + (m_useInputBias ? 1 : 0), -1.8, m_intrinsicDims);
			GVec::capValues(pPrefVec + (m_useInputBias ? 1 : 0), 1.8, m_intrinsicDims);
		}

		// Stopping criteria
		double rsse = sqrt(validate(data));
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.001) // If the amount of improvement is less than 0.1%
			learningRate *= m_decayRate; // decay the learning rate
		prevErr = rsse;
	}

	// Impute missing values
	size_t n = std::min(dims, m_pQ->rows());
	size_t i;
	for(i = 0; i < n; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
		{
			double* pWeights = m_pQ->row(i);
			double* pPref = pPrefVec;
			double pred = *(pWeights++);
			if(m_useInputBias)
				pred += *(pPref++);
			for(size_t j = 0; j < m_intrinsicDims; j++)
				pred += *(pPref++) * (*pWeights++);
			*pVec = pred;
		}
		pVec++;
	}
	for( ; i < dims; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			*pVec = 0.0;
		pVec++;
	}
}

#ifndef NO_TEST_CODE
// static
void GMatrixFactorization::test()
{
	GMatrixFactorization rec(3);
	rec.setRegularizer(0.002);
	rec.basicTest(0.17);
}
#endif







GHybridNonlinearPCA::GHybridNonlinearPCA(size_t intrinsicDims)
: GNonlinearPCA(intrinsicDims), m_itemAttrs(NULL)
{
}

// virtual
GHybridNonlinearPCA::~GHybridNonlinearPCA()
{
}

// virtual
void GHybridNonlinearPCA::train(GMatrix& data)
{
	size_t users, items;
	GCollaborativeFilter_dims(data, &items, &users);
	m_items = items;

	// Copy and normalize the ratings
	GMatrix* pClone = new GMatrix();
	pClone->copy(&data);
	Holder<GMatrix> hClone(pClone);
	delete[] m_pMins;
	m_pMins = new double[items];
	delete[] m_pMaxs;
	m_pMaxs = new double[items];
	GVec::setAll(m_pMins, 1e200, items);
	GVec::setAll(m_pMaxs, -1e200, items);
	for(size_t i = 0; i < pClone->rows(); i++)
	{
		double* pVec = pClone->row(i);
		m_pMins[size_t(pVec[0])] = std::min(m_pMins[size_t(pVec[0])], pVec[2]);
		m_pMaxs[size_t(pVec[0])] = std::max(m_pMaxs[size_t(pVec[0])], pVec[2]);
	}
	for(size_t i = 0; i < items; i++)
	{
		if(m_pMins[i] >= 1e200)
			m_pMins[i] = 0.0;
		if(m_pMaxs[i] < m_pMins[i] + 1e-12)
			m_pMaxs[i] = m_pMins[i] + 1.0;
	}
	for(size_t i = 0; i < pClone->rows(); i++)
	{
		double*  pVec = pClone->row(i);
		pVec[2] = (pVec[2] - m_pMins[size_t(pVec[0])]) / (m_pMaxs[size_t(pVec[0])] - m_pMins[size_t(pVec[0])]);
	}

	// Prep the model for incremental training
	size_t numAttr = m_itemAttrs->cols() - 1;
	GUniformRelation featureRel(m_intrinsicDims + numAttr);
	GUniformRelation labelRel(items);
	m_pModel->setUseInputBias(m_useInputBias);
	m_pModel->beginIncrementalLearning(featureRel, labelRel);
	GNeuralNet nn;
	nn.addLayer(new GLayerClassic(FLEXIBLE_SIZE, FLEXIBLE_SIZE));
	nn.setUseInputBias(m_useInputBias);
	nn.beginIncrementalLearning(featureRel, labelRel);
	double* pPrefGradient = new double[m_intrinsicDims + numAttr];
	ArrayHolder<double> hPrefGradient(pPrefGradient);

	// Train
	size_t startPass = 0;
	if(!m_useThreePass)
		startPass = 2;
	else if(m_pModel->layerCount() == 1)
		startPass = 2;
	for(size_t pass = startPass; pass < 3; pass++)
	{
		GNeuralNet* pNN = (pass == 0 ? &nn : m_pModel);
		if(pass == startPass)
		{
			// Initialize the user matrix
			delete(m_pUsers);
			m_pUsers = new GMatrix(users, m_intrinsicDims + numAttr);
			size_t count = 0;
			double* itemVec = m_itemAttrs->row(count);
			for(size_t i = 0; i < users; i++)
			{
				double* pVec = m_pUsers->row(i);
				GVec::setAll(pVec, 0, m_intrinsicDims + numAttr);
				for(size_t j = 0; j < m_intrinsicDims; j++)
					*(pVec++) = 0.01 * m_rand.normal();
				if(*itemVec == i)
				{
					itemVec++;
					for(size_t j = 1; j < numAttr; j++)
						*(pVec++) = *(itemVec++) * 0.01;
					itemVec = m_itemAttrs->row(++count);
				}
				
			}
		}
		double rateBegin = 0.1;
		double rateEnd = 0.001;
		double prevErr = 1e308;
		for(double learningRate = rateBegin; learningRate > rateEnd; )
		{
			for(size_t j = 0; j < m_minIters; j++)
			{
				// Shuffle the ratings
				pClone->shuffle(m_rand);

				// Do an epoch of training
				m_pModel->setLearningRate(learningRate);
				for(size_t i = 0; i < pClone->rows(); i++)
				{
					// Forward-prop
					double* pVec = pClone->row(i);
					size_t user = size_t(pVec[1]);
					size_t item = size_t(pVec[0]);
					double* pPrefs = m_pUsers->row(user);
					pNN->forwardPropSingleOutput(pPrefs, item);

					// Update weights
					pNN->backpropagateSingleOutput(item, pVec[2]);
					if(pass < 2)
						pNN->scaleWeightsSingleOutput(item, 1.0 - (learningRate * m_regularizer));
					if(pass != 1)
						pNN->gradientOfInputsSingleOutput(item, pPrefGradient);
					pNN->descendGradientSingleOutput(item, pPrefs, learningRate, pNN->momentum());
					if(pass != 1)
					{
						// Update inputs
						if(pass == 0)
							GVec::multiply(pPrefs, 1.0 - (learningRate * m_regularizer), m_intrinsicDims);
						GVec::addScaled(pPrefs, -learningRate, pPrefGradient, m_intrinsicDims);
//						GVec::floorValues(pPrefs, -1.0, m_intrinsicDims);
//						GVec::capValues(pPrefs, 1.0, m_intrinsicDims);
					}
				}
			}

			// Stopping criteria
			double rmse = sqrt(validate(pNN, *pClone));
			if(rmse < 1e-12 || 1.0 - (rmse / prevErr) < 0.001) // If the amount of improvement is small
				learningRate *= m_decayRate; // decay the learning rate
			prevErr = rmse;
		}
	}
}

// virtual
double GHybridNonlinearPCA::predict(size_t item, size_t user)
{
	if(user >= m_pUsers->rows() || item >= m_items)
		return 0.0;
	else
		return (m_pMaxs[item] - m_pMins[item]) * m_pModel->forwardPropSingleOutput(m_pUsers->row(user), item) + m_pMins[item];
}

double GHybridNonlinearPCA::validate(GNeuralNet* pNN, GMatrix& data)
{
	double sse = 0;
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data[i];
		double* pPrefs = m_pUsers->row(size_t(pVec[1]));
		double predictedRating = pNN->forwardPropSingleOutput(pPrefs, size_t(pVec[0]));
		double d = pVec[2] - predictedRating;
		sse += (d * d);
	}
	return sse / data.rows();
}

void GHybridNonlinearPCA::setItemAttributes(GMatrix& itemAttrs)
{
	delete(m_itemAttrs);
	m_itemAttrs = new GMatrix();
	m_itemAttrs->copy(&itemAttrs);
}







GNonlinearPCA::GNonlinearPCA(size_t intrinsicDims)
: GCollaborativeFilter(), m_intrinsicDims(intrinsicDims), m_items(0), m_pMins(NULL), m_pMaxs(NULL), m_useInputBias(true), m_useThreePass(true), m_minIters(1), m_decayRate(0.97), m_regularizer(0.0001)
{
	m_pModel = new GNeuralNet();
	m_pUsers = NULL;
}

GNonlinearPCA::GNonlinearPCA(GDomNode* pNode, GLearnerLoader& ll)
: GCollaborativeFilter(pNode, ll)
{
	m_useInputBias = pNode->field("uib")->asBool();
	m_pUsers = new GMatrix(pNode->field("users"));
	m_pModel = new GNeuralNet(pNode->field("model"), ll);
	m_items = m_pModel->layer(m_pModel->layerCount() - 1).outputs();
	m_pMins = new double[m_items];
	GDomListIterator it1(pNode->field("mins"));
	if(it1.remaining() != m_items)
		throw Ex("invalid number of elements");
	GVec::deserialize(m_pMins, it1);
	m_pMaxs = new double[m_items];
	GDomListIterator it2(pNode->field("maxs"));
	if(it2.remaining() != m_items)
		throw Ex("invalid number of elements");
	GVec::deserialize(m_pMaxs, it2);
	m_intrinsicDims = m_pModel->layer(0).outputs();
}

// virtual
GNonlinearPCA::~GNonlinearPCA()
{
	delete[] m_pMins;
	delete[] m_pMaxs;
	delete(m_pModel);
	delete(m_pUsers);
}

// virtual
GDomNode* GNonlinearPCA::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GNonlinearPCA");
	pNode->addField(pDoc, "uib", pDoc->newBool(m_useInputBias));
	pNode->addField(pDoc, "users", m_pUsers->serialize(pDoc));
	pNode->addField(pDoc, "model", m_pModel->serialize(pDoc));
	size_t itemCount = m_pModel->outputLayer().outputs();
	pNode->addField(pDoc, "mins", GVec::serialize(pDoc, m_pMins, itemCount));
	pNode->addField(pDoc, "maxs", GVec::serialize(pDoc, m_pMaxs, itemCount));
	return pNode;
}

double GNonlinearPCA::validate(GNeuralNet* pNN, GMatrix& data)
{
	double sse = 0;
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data[i];
		double* pPrefs = m_pUsers->row(size_t(pVec[0]));
		double predictedRating = pNN->forwardPropSingleOutput(pPrefs, size_t(pVec[1]));
		double d = pVec[2] - predictedRating;
		sse += (d * d);
	}
	return sse / data.rows();
}

// virtual
void GNonlinearPCA::train(GMatrix& data)
{
	size_t users, items;
	GCollaborativeFilter_dims(data, &users, &items);
	m_items = items;

	// Copy and normalize the ratings
	GMatrix* pClone = new GMatrix();
	pClone->copy(&data);
	Holder<GMatrix> hClone(pClone);
	delete[] m_pMins;
	m_pMins = new double[items];
	delete[] m_pMaxs;
	m_pMaxs = new double[items];
	GVec::setAll(m_pMins, 1e200, items);
	GVec::setAll(m_pMaxs, -1e200, items);
	for(size_t i = 0; i < pClone->rows(); i++)
	{
		double* pVec = pClone->row(i);
		m_pMins[size_t(pVec[1])] = std::min(m_pMins[size_t(pVec[1])], pVec[2]);
		m_pMaxs[size_t(pVec[1])] = std::max(m_pMaxs[size_t(pVec[1])], pVec[2]);
	}
	for(size_t i = 0; i < items; i++)
	{
		if(m_pMins[i] >= 1e200)
			m_pMins[i] = 0.0;
		if(m_pMaxs[i] < m_pMins[i] + 1e-12)
			m_pMaxs[i] = m_pMins[i] + 1.0;
	}
	for(size_t i = 0; i < pClone->rows(); i++)
	{
		double*  pVec = pClone->row(i);
		pVec[2] = (pVec[2] - m_pMins[size_t(pVec[1])]) / (m_pMaxs[size_t(pVec[1])] - m_pMins[size_t(pVec[1])]);
	}

	// Prep the model for incremental training
	GUniformRelation featureRel(m_intrinsicDims);
	GUniformRelation labelRel(items);
	m_pModel->setUseInputBias(m_useInputBias);
	m_pModel->beginIncrementalLearning(featureRel, labelRel);
	GNeuralNet nn;
	nn.addLayer(new GLayerClassic(FLEXIBLE_SIZE, FLEXIBLE_SIZE));
	nn.setUseInputBias(m_useInputBias);
	nn.beginIncrementalLearning(featureRel, labelRel);
	double* pPrefGradient = new double[m_intrinsicDims];
	ArrayHolder<double> hPrefGradient(pPrefGradient);

	// Train
	size_t startPass = 0;
	if(!m_useThreePass)
		startPass = 2;
	else if(m_pModel->layerCount() == 1)
		startPass = 2;
	for(size_t pass = startPass; pass < 3; pass++)
	{
		GNeuralNet* pNN = (pass == 0 ? &nn : m_pModel);
		if(pass == startPass)
		{
			// Initialize the user matrix
			delete(m_pUsers);
			m_pUsers = new GMatrix(users, m_intrinsicDims);
			for(size_t i = 0; i < users; i++)
			{
				double* pVec = m_pUsers->row(i);
				for(size_t j = 0; j < m_intrinsicDims; j++)
					*(pVec++) = 0.01 * m_rand.normal();
			}
		}
		double rateBegin = 0.1;
		double rateEnd = 0.001;
		double prevErr = 1e308;
		for(double learningRate = rateBegin; learningRate > rateEnd; )
		{
			for(size_t j = 0; j < m_minIters; j++)
			{
				// Shuffle the ratings
				pClone->shuffle(m_rand);

				// Do an epoch of training
				m_pModel->setLearningRate(learningRate);
				for(size_t i = 0; i < pClone->rows(); i++)
				{
					// Forward-prop
					double* pVec = pClone->row(i);
					size_t user = size_t(pVec[0]);
					size_t item = size_t(pVec[1]);
					double* pPrefs = m_pUsers->row(user);
					pNN->forwardPropSingleOutput(pPrefs, item);

					// Update weights
					pNN->backpropagateSingleOutput(item, pVec[2]);
					if(pass < 2)
						pNN->scaleWeightsSingleOutput(item, 1.0 - (learningRate * m_regularizer));
					if(pass != 1)
						pNN->gradientOfInputsSingleOutput(item, pPrefGradient);
					pNN->descendGradientSingleOutput(item, pPrefs, learningRate, pNN->momentum());
					if(pass != 1)
					{
						// Update inputs
						if(pass == 0)
							GVec::multiply(pPrefs, 1.0 - (learningRate * m_regularizer), m_intrinsicDims);
						GVec::addScaled(pPrefs, -learningRate, pPrefGradient, m_intrinsicDims);
//						GVec::floorValues(pPrefs, -1.0, m_intrinsicDims);
//						GVec::capValues(pPrefs, 1.0, m_intrinsicDims);
					}
				}
			}

			// Stopping criteria
			double rmse = sqrt(validate(pNN, *pClone));
			if(rmse < 1e-12 || 1.0 - (rmse / prevErr) < 0.001) // If the amount of improvement is small
				learningRate *= m_decayRate; // decay the learning rate
			prevErr = rmse;
		}
	}
}

// virtual
double GNonlinearPCA::predict(size_t user, size_t item)
{
	if(user >= m_pUsers->rows() || item >= m_items)
		return 0.0;
	else
		return (m_pMaxs[item] - m_pMins[item]) * m_pModel->forwardPropSingleOutput(m_pUsers->row(user), item) + m_pMins[item];
}

// virtual
void GNonlinearPCA::impute(double* pVec, size_t dims)
{
	throw Ex("Sorry, GNonlinearPCA::impute is not implemented yet");
/*	// Initialize a preference vector
	GTEMPBUF(double, pPrefVec, m_intrinsicDims);
	GActivationFunction* pAF = m_pModel->layer(0).m_pActivationFunction;
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pPrefVec[i] = pAF->center() + 0.25 * m_pRand->normal();

	// Make a single list of all the ratings
	size_t itemCount = m_pModel->layer(m_pModel->layerCount() - 1).m_neurons.size();
	GHeap heap(2048);
	vector<Rating*> ratings;
	GMatrixFactorization_vectorToRatings(pVec, itemCount, heap, ratings, *m_pRand);
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		pRating->m_rating = (pRating->m_rating - m_pMins[pRating->m_item]) / (m_pMaxs[pRating->m_item] - m_pMins[pRating->m_item]);
	}

	// Refine the preference vector
	double prevErr = 1e308;
	double learningRate = 0.2;
	while(learningRate >= 0.01)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_pRand->next(n)], ratings[n - 1]);

		// Do an epoch of training
		m_pModel->setLearningRate(learningRate);
		double sse = 0;
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;
			double predictedRating = m_pModel->forwardPropSingleOutput(pPrefVec, pRating->m_item);
			double d = pRating->m_rating - predictedRating;
			sse += (d * d);
			m_pModel->setErrorSingleOutput(pRating->m_rating, pRating->m_item, m_pModel->backPropTargetFunction());
			m_pModel->backProp()->backpropagateSingleOutput(pRating->m_item);
			m_pModel->backProp()->adjustFeaturesSingleOutput(pRating->m_item, pPrefVec, learningRate, m_pModel->useInputBias());
		}

		// Stopping criteria
		double rsse = sqrt(sse);
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.0001) // If the amount of improvement is less than 0.01%
			learningRate *= 0.8; // decay the learning rate
		prevErr = rsse;
	}

	// Impute missing values
	for(size_t i = 0; i < itemCount; i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
			pVec[i] = (m_pMaxs[i] - m_pMins[i]) * m_pModel->forwardPropSingleOutput(pPrefVec, i) + m_pMins[i];
	}*/
}

#ifndef NO_TEST_CODE
// static
void GNonlinearPCA::test()
{
	GNonlinearPCA rec(3);
	rec.model()->addLayer(new GLayerClassic(FLEXIBLE_SIZE, 3));
	rec.model()->addLayer(new GLayerClassic(3, FLEXIBLE_SIZE));
	rec.basicTest(0.261);
}
#endif










GBagOfRecommenders::GBagOfRecommenders()
: GCollaborativeFilter(), m_itemCount(0)
{
}

GBagOfRecommenders::GBagOfRecommenders(GDomNode* pNode, GLearnerLoader& ll)
: GCollaborativeFilter(pNode, ll)
{
	m_itemCount = (size_t)pNode->field("ic")->asInt();
	for(GDomListIterator it(pNode->field("filters")); it.current(); it.advance())
		m_filters.push_back(ll.loadCollaborativeFilter(it.current()));
}

GBagOfRecommenders::~GBagOfRecommenders()
{
	clear();
}

void GBagOfRecommenders::clear()
{
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
		delete(*it);
	m_filters.clear();
}

void GBagOfRecommenders::addRecommender(GCollaborativeFilter* pRecommender)
{
	pRecommender->rand().setSeed(m_rand.next()); // Ensure that each recommender has a different seed
	m_filters.push_back(pRecommender);
}

// virtual
void GBagOfRecommenders::train(GMatrix& data)
{
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
	{
		// Make a matrix that randomly samples about half of the elements in pData
		GMatrix tmp(data.relation().clone());
		GReleaseDataHolder hTmp(&tmp);
		for(size_t i = 0; i < data.rows(); i++)
		{
			if(m_rand.next(2) == 0)
				tmp.takeRow(data[i]);
		}

		// Train with it
		(*it)->train(tmp);
	}
}

// virtual
double GBagOfRecommenders::predict(size_t user, size_t item)
{
	double sum = 0.0;
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
		sum += (*it)->predict(user, item);
	return sum / m_filters.size();
}

// virtual
void GBagOfRecommenders::impute(double* pVec, size_t dims)
{
	size_t n = std::min(m_itemCount, dims);
	GTEMPBUF(double, pBuf1, n);
	GTEMPBUF(double, pBuf2, n);
	GVec::setAll(pBuf2, 0.0, n);
	double count = 0.0;
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
	{
		GVec::copy(pBuf1, pVec, n);
		(*it)->impute(pBuf1, dims);
		GVec::multiply(pBuf2, count / (count + 1), n);
		GVec::addScaled(pBuf2, 1.0 / (count + 1), pBuf1, n);
		count++;
	}
	size_t i;
	for(i = 0; i < n; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			*pVec = pBuf2[i];
		pVec++;
	}
	for( ; i < dims; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			*pVec = 0.0;
		pVec++;
	}
}

// virtual
GDomNode* GBagOfRecommenders::serialize(GDom* pDoc) const
{
	GDomNode* pNode = baseDomNode(pDoc, "GBagOfRecommenders");
	pNode->addField(pDoc, "ic", pDoc->newInt(m_itemCount));
	GDomNode* pFilters = pNode->addField(pDoc, "filters", pDoc->newList());
	for(vector<GCollaborativeFilter*>::const_iterator it = m_filters.begin(); it != m_filters.end(); it++)
		pFilters->addItem(pDoc, (*it)->serialize(pDoc));
	return pNode;
}

#ifndef NO_TEST_CODE
// static
void GBagOfRecommenders::test()
{
	GBagOfRecommenders rec;
	rec.addRecommender(new GBaselineRecommender());
	rec.addRecommender(new GMatrixFactorization(3));
	GNonlinearPCA* nlpca = new GNonlinearPCA(3);
	nlpca->model()->addLayer(new GLayerClassic(FLEXIBLE_SIZE, FLEXIBLE_SIZE));
	rec.addRecommender(nlpca);
	rec.basicTest(0.57);
}
#endif






//virtual
GContentBasedFilter::~GContentBasedFilter()
{
	delete(m_itemAttrs);
	clear();
}

//virtual
void GContentBasedFilter::train(GMatrix& data)
{
	clear();
	m_userMap.clear();
	m_userRatings.clear();

	size_t users, items;
        GCollaborativeFilter_dims(data, &users, &items);
        m_items = items;
	m_users = users;
	std::set<size_t> userSet;
	

	if(m_itemAttrs == NULL)
		throw Ex("The items attributes has to be set");	

	//create a training set and learning algorithm for each user
	for(size_t i = 0; i < data.rows(); i++)
	{
		double* pVec = data.row(i);
		m_userRatings.insert(std::make_pair((size_t)pVec[0], (size_t)pVec[1]));
		userSet.insert((size_t)pVec[0]);
	}

	//Loop through the set of users
	for(std::set<size_t>::iterator it = userSet.begin(); it != userSet.end(); ++it)
	{
		m_args.set_pos(m_init_pos);
		pair<multimap<size_t, size_t>::iterator, multimap<size_t, size_t>::iterator> ratedItems;
		ratedItems = m_userRatings.equal_range(*it);

		//create the training data for the user
		GMatrix* trainingData = new GMatrix(m_itemAttrs->relation().clone());
		GRelation* relation = data.relation().cloneSub(data.cols() - 1, 1);
		GMatrix* labels = new GMatrix(relation);
		for(multimap<size_t, size_t>::iterator ratings = ratedItems.first; ratings != ratedItems.second; ++ratings)
		{
			trainingData->copyRow(m_itemAttrs->row(m_itemMap[(*ratings).second]));

			double* temp = labels->newRow();
			temp[0] = data[(*ratings).second][2];
		}

		//train a learning algorithm for each user
		GSupervisedLearner* pLearn = (GSupervisedLearner*)GLearnerLib::InstantiateAlgorithm(m_args, trainingData, labels);
	        if(m_args.size() > 0)
	                throw Ex("Superfluous argument: ", m_args.peek());
		pLearn->train(*trainingData, *labels);
		m_userMap[(*it)] = m_learners.size();
		m_learners.push_back(pLearn);
	}
}

//virtual
double GContentBasedFilter::predict(size_t user, size_t item)
{
	if(user >= m_users || item >= m_items)
                return 0.0;
	double pOut[1];
	m_learners[m_userMap[user]]->predict(m_itemAttrs->row(m_itemMap[item]), pOut);
	return pOut[0];
}

//virtual
void GContentBasedFilter::impute(double* pVec, size_t dims)
{
/*
	for(size_t i = 0; i < dims; i++)
	{
		if(*pVec == UNKNOWN_REAL_VALUE)
			(*pVec) = m_learners[]
	}
*/
	std::cerr << "Not yet implemented\n";
}

//virtual
GDomNode* GContentBasedFilter::serialize(GDom* pDoc) const
{
	return NULL;
}

void GContentBasedFilter::clear()
{
//        for(vector<GSupervisedLearner*>::iterator it = m_learners.begin(); it != m_learners.end(); it++)
//                delete(*it);
        m_learners.clear();
}

void GContentBasedFilter::setItemAttributes(GMatrix& itemAttrs)
{
	delete(m_itemAttrs);
	m_itemAttrs = new GMatrix();
	m_itemAttrs->copy(&itemAttrs);
	for(size_t i = 0; i < m_itemAttrs->rows(); i++)
	{
		double* pVec = m_itemAttrs->row(i);
		m_itemMap[(size_t)pVec[0]] = i;
	}
	m_itemAttrs->swapColumns(0,m_itemAttrs->cols()-1);
	m_itemAttrs->deleteColumn(m_itemAttrs->cols()-1);
}






GContentBoostedCF::GContentBoostedCF(GArgReader copy)
: GCollaborativeFilter(), m_ratingCounts(NULL), m_pseudoRatingSum(NULL)
{
	int orig_argc = copy.get_argc();
	int orig_pos = copy.get_pos();
	while(strcmp(copy.pop_string(), "--") != 0)
		if(copy.size() == 0)
			throw Ex("Expecting \"--\" to denote the parameters for the instance-based CF\n");
	int dashLoc = copy.get_pos() - 1;
	copy.set_argc(dashLoc);
	copy.set_pos(orig_pos);
	m_cbf = GRecommenderLib::InstantiateContentBasedFilter(copy);
	copy.set_pos(dashLoc + 1);
	copy.set_argc(orig_argc);
	m_cf = GRecommenderLib::InstantiateInstanceRecommender(copy);
}

GContentBoostedCF::~GContentBoostedCF()
{
	delete(m_cbf);
	delete(m_cf);
	delete[] m_ratingCounts;
	delete[] m_pseudoRatingSum;
}

void GContentBoostedCF::train(GMatrix& data)
{
	//make a copy of the training data
	GMatrix* pClone = new GMatrix();
        pClone->copy(&data);
        Holder<GMatrix> hClone(pClone);
	m_cbf->train(*pClone);

	//Create the psuedo user-ratings vector for every user
	m_userMap = m_cbf->getUserMap();
	map<size_t, size_t> items = m_cbf->getItemMap();
	multimap<size_t, size_t> userRatings = m_cbf->getUserRatings();
	delete[] m_ratingCounts;
	delete[] m_pseudoRatingSum;
	m_ratingCounts = new size_t[m_userMap.size()];
	GIndexVec::setAll(m_ratingCounts, 0, m_userMap.size());

	m_pseudoRatingSum = new double[m_userMap.size()];
	GVec::setAll(m_pseudoRatingSum, 0.0, m_userMap.size());

	for(size_t i = 0; i < pClone->rows(); i++)
        {
		double* pVec = pClone->row(i);
		m_pseudoRatingSum[m_userMap[(size_t)pVec[0]]] += pVec[2];
	}

	//Loop through all of the users
	for(map<size_t, size_t>::iterator user=m_userMap.begin(); user!=m_userMap.end(); ++user)
	{
		pair<multimap<size_t, size_t>::iterator, multimap<size_t, size_t>::iterator> ratings;
		ratings = userRatings.equal_range(user->first);

		//Loop through all of the items
		for(map<size_t, size_t>::iterator item=items.begin(); item!=items.end(); ++item)
		{
			//Check if user has rated item
			bool isRated = false;
			multimap<size_t, size_t>::iterator rating;
			for(rating=ratings.first; rating!=ratings.second; ++rating)
			{
				if(rating->second == item->first)
					isRated = true;
				m_ratingCounts[m_userMap[user->first]]++;
			}
			if(!isRated)
			{
				//make prediction
				double* pRating = pClone->newRow();
				pRating[0] = user->first;
				pRating[1] = item->first;
				pRating[2] = m_cbf->predict(user->first, item->first);
				GAssert(pRating[2] != UNKNOWN_REAL_VALUE);
				m_pseudoRatingSum[m_userMap[user->first]] += pRating[2];
			}
		}
	}

	//Train CF on the psuedo user-ratings
	m_cf->train(*pClone);
	m_cf->clearUserDEPQ();
}

double GContentBoostedCF::predict(size_t user, size_t item)
{
	double max = 2;
	multimap<double,ArrayWrapper> neighbors = m_cf->getNeighbors(user, item);

        // Combine the ratings of the nearest neighbors to make a prediction
	size_t num = m_ratingCounts[m_userMap[user]];
	double selfWeight = (num > 50) ? 1 : num / 50;
        double weighted_sum = max * selfWeight * (m_cbf->predict(user, item)); // - (m_pseudoRatingSum[m_userMap[user]] / m_ratingCounts[m_userMap[user]]));
        double sum_weight = max * selfWeight;
        for(multimap<double,ArrayWrapper>::iterator it = neighbors.begin(); it != neighbors.end(); it++)
        {
                double weight = std::max(0.0, std::min(1.0, it->first));
		size_t neighNum = m_ratingCounts[m_userMap[(size_t)it->first]];
		double neighWeight = (neighNum > 50) ? 1 : neighNum / 50;
		double sigWeight = (it->second.values[1] > 50) ? 1 : it->second.values[1] / 50;
		weight *= ((2 * selfWeight * neighWeight) / (selfWeight + neighWeight)) + sigWeight;
                double val = m_cf->getRating(it->second.values[0], item);
                weighted_sum += weight * val;
                sum_weight += weight;
        }

//	return (m_pseudoRatingSum[m_userMap[user]] / m_ratingCounts[m_userMap[user]]) + (weighted_sum / sum_weight);
	return weighted_sum / sum_weight;
}

void GContentBoostedCF::impute(double* pVec, size_t dims)
{
	std::cerr << "Not yet implemented\n";
}




} // namespace GClasses
