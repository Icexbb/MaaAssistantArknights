#include "Matcher.h"

#include "Utils/NoWarningCV.h"

#include "Config/TaskData.h"
#include "Config/TemplResource.h"
#include "Utils/Logger.hpp"
#include "Utils/StringMisc.hpp"

using namespace asst;

Matcher::ResultOpt Matcher::analyze() const
{
    const auto match_results = preproc_and_match(make_roi(m_image, m_roi), m_params);

    for (size_t i = 0; i < match_results.size(); ++i) {
        const auto& [matched, templ, templ_name] = match_results[i];
        if (matched.empty()) {
            continue;
        }

        double min_val = 0.0, max_val = 0.0;
        cv::Point min_loc, max_loc;
        cv::minMaxLoc(matched, &min_val, &max_val, &min_loc, &max_loc);

        Rect rect(max_loc.x + m_roi.x, max_loc.y + m_roi.y, templ.cols, templ.rows);
        if (std::isnan(max_val) || std::isinf(max_val)) {
            max_val = 0;
        }
        if (m_log_tracing && max_val > 0.5) { // 得分太低的肯定不对，没必要打印
            Log.trace("match_templ |", templ_name, "score:", max_val, "rect:", rect, "roi:", m_roi);
        }

        double threshold = m_params.templ_thres[i];
        if (max_val < threshold) {
            continue;
        }

        // FIXME: 老接口太难重构了，先弄个这玩意兼容下，后续慢慢全删掉
        m_result.rect = rect;
        m_result.score = max_val;
        m_result.templ_name = templ_name;
        return m_result;
    }

    return std::nullopt;
}

std::vector<Matcher::RawResult> Matcher::preproc_and_match(const cv::Mat& image, const MatcherConfig::Params& params)
{
    std::vector<Matcher::RawResult> results;
    for (size_t i = 0; i != params.templs.size(); ++i) {
        const auto& ptempl = params.templs[i];
        auto method = MatchMethod::Ccoeff;
        if (params.methods.size() <= i) {
            Log.warn("methods is empty, use default method: Ccoeff");
        }
        else {
            method = params.methods[i];
        }

        if (method == MatchMethod::Invalid) {
            Log.error(__FUNCTION__, "| invalid method");
            return {};
        }

        cv::Mat templ;
        std::string templ_name;

        if (std::holds_alternative<std::string>(ptempl)) {
            templ_name = std::get<std::string>(ptempl);
            templ = TemplResource::get_instance().get_templ(templ_name);
        }
        else if (std::holds_alternative<cv::Mat>(ptempl)) {
            templ = std::get<cv::Mat>(ptempl);
        }
        else {
            Log.error("templ is none");
        }

        if (templ.empty()) {
            Log.error("templ is empty!", templ_name);
#ifdef ASST_DEBUG
            throw std::runtime_error("templ is empty: " + templ_name);
#else
            return {};
#endif
        }

        if (templ.cols > image.cols || templ.rows > image.rows) {
            Log.error("templ size is too large", templ_name, "image size:", image.cols, image.rows,
                      "templ size:", templ.cols, templ.rows);
            return {};
        }

        cv::Mat matched;
        cv::Mat image_for_match;
        cv::Mat templ_for_match;
        cv::Mat image_for_count;
        cv::Mat templ_for_count;
        cv::cvtColor(image, image_for_match, cv::COLOR_BGR2RGB);
        cv::cvtColor(templ, templ_for_match, cv::COLOR_BGR2RGB);
        if (method == MatchMethod::HSVCount) {
            cv::cvtColor(image, image_for_count, cv::COLOR_BGR2HSV);
            cv::cvtColor(templ, templ_for_count, cv::COLOR_BGR2HSV);
        }
        else if (method == MatchMethod::RGBCount) {
            image_for_count = image_for_match;
            templ_for_count = templ_for_match;
        }

        // 目前所有的匹配都是用 TM_CCOEFF_NORMED
        int match_algorithm = cv::TM_CCOEFF_NORMED;

        auto calc_mask = [&](const cv::Mat& templ_for_mask, bool with_close)->std::optional<cv::Mat> {
            cv::Mat templ_for_gray_mask;
            cv::cvtColor(templ_for_mask, templ_for_gray_mask, cv::COLOR_BGR2GRAY);

            // Union all masks, not intersection
            cv::Mat mask = cv::Mat::zeros(templ_for_gray_mask.size(), CV_8UC1);
            for (const auto& range : params.mask_range) {
                cv::Mat current_mask;
                if (range.first.size() == 1 && range.second.size() == 1) {
                    cv::inRange(templ_for_gray_mask, range.first[0], range.second[0], current_mask);
                }
                else if (range.first.size() == 3 && range.second.size() == 3) {
                    cv::inRange(templ_for_mask, range.first, range.second, current_mask);
                }
                else {
                    Log.error("Invalid mask range");
                    return std::nullopt;
                }
                cv::bitwise_or(mask, current_mask, mask);
            }

            if (with_close) {
                cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
                cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
            }
            return mask;
        };

        if (params.mask_range.empty() || method == MatchMethod::RGBCount || method == MatchMethod::HSVCount) {
            // workaround: 数色时的模板匹配忽略 maskRange
            // TODO: 区分 maskRange 和 colorRange
            cv::matchTemplate(image_for_match, templ_for_match, matched, match_algorithm);
        }
        else {
            auto mask_opt = calc_mask(
                params.mask_with_src ? image_for_match : templ_for_match,
                params.mask_with_close);
            if (!mask_opt) {
                return {};
            }
            cv::matchTemplate(image_for_match, templ_for_match, matched, match_algorithm, mask_opt.value());
        }

        if (method == MatchMethod::RGBCount || method == MatchMethod::HSVCount) {
            auto templ_active_opt = calc_mask(templ_for_count, false);
            auto image_active_opt = calc_mask(image_for_count, false);
            if (!image_active_opt || !templ_active_opt) [[unlikely]] {
                return {};
            }
            const auto& templ_active = templ_active_opt.value();
            const auto& image_active = image_active_opt.value();
            cv::threshold(templ_active, templ_active, 1, 1, cv::THRESH_BINARY);
            cv::threshold(image_active, image_active, 1, 1, cv::THRESH_BINARY);
            // 把 CCORR 当 count 用，计算 image_active 在 templ_active 形状内的像素数量
            cv::Mat tp, fp;
            int tp_fn = cv::countNonZero(templ_active);
            cv::matchTemplate(image_active, templ_active, tp, cv::TM_CCORR);
            tp.convertTo(tp, CV_32S);
            cv::Mat templ_inactive = 1 - templ_active;
            // TODO: 这里 TP+FP 是 image_active 的 count，可以消掉一个 matchtemplate
            cv::matchTemplate(image_active, templ_inactive, fp, cv::TM_CCORR);
            fp.convertTo(fp, CV_32S);
            cv::Mat count_result; // 数色结果为 f1_score
            cv::divide(2 * tp, tp + fp + tp_fn, count_result, 1, CV_32F);
            // 返回的是数色和模板匹配的点积
            cv::multiply(matched, count_result, matched);
        }
        results.emplace_back(RawResult { .matched = matched, .templ = templ, .templ_name = templ_name });
    }
    return results;
}
