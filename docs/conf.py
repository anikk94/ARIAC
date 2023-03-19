# Configuration file for the Sphinx documentation builder.

# -- Project information

project = 'ARIAC'
copyright = 'NIST'
author = 'Pavel'

release = '0.1'
version = '0.1.0'



# -- General configuration

extensions = [
    'myst_parser',
    'sphinx.ext.mathjax',
    'sphinx_rtd_theme',
    'sphinx.ext.autosectionlabel',
    # External stuff
    "myst_parser",
    "sphinx_copybutton",
]

templates_path = ['_templates']

# Make sure the target is unique
autosectionlabel_prefix_document = True

intersphinx_mapping = {
    'python': ('https://docs.python.org/3/', None),
    'sphinx': ('https://www.sphinx-doc.org/en/master/', None),
}
intersphinx_disabled_domains = ['std']





html_theme = 'sphinx_rtd_theme'
numfig = True
# The name of the Pygments (syntax highlighting) style to use.

pygments_style = 'tango'

source_suffix = ['.rst', '.md']

html_static_path = ['custom']

html_css_files = [
    'css/custom.css',
]

html_js_files = [
    'js/custom.js'
]

# -- Options for copy button -------------------------------------------------------
#
copybutton_prompt_text = r">>> |\.\.\. |\$ |In \[\d*\]: | {2,5}\.\.\.: | {5,8}: "
copybutton_prompt_is_regexp = True
copybutton_line_continuation_character = "\\"
copybutton_here_doc_delimiter = "EOT"

copybutton_selector = "div:not(.no-copybutton) > div.highlight > pre"

# -- Options for EPUB output
epub_show_urls = 'footnote'